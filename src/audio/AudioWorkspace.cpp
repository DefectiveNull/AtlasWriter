#include "audio/AudioWorkspace.h"

#include "audio/AudioDecoder.h"
#include "embed/Embed.h"
#include "text/Font.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <mutex>
#include <thread>

namespace {

enum ToolId : int {
    kToolPlay = 1,
    kToolSpeed,
    kToolVolumeDown,
    kToolVolumeUp,
};

constexpr float kSpeeds[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
constexpr size_t kNormalSpeed = 2;
constexpr uint64_t kFramesPerPeak = 1024; // ~23 ms at 44.1 kHz
constexpr double kSeekSeconds = 5.0;

std::string clock(double seconds)
{
    const long long total = std::max(0LL, static_cast<long long>(seconds));
    char text[32];
    if (total >= 3600)
        SDL_snprintf(text, sizeof(text), "%lld:%02lld:%02lld", total / 3600, total / 60 % 60, total % 60);
    else
        SDL_snprintf(text, sizeof(text), "%lld:%02lld", total / 60, total % 60);
    return text;
}

std::string speedLabel(float speed)
{
    char text[16];
    SDL_snprintf(text, sizeof(text), "%gx", static_cast<double>(speed));
    return text;
}

} // namespace

// ---------------------------------------------------------------------------
// One open file: a decoder feeding an SDL audio stream, and a second one
// reading the whole file once on a worker for the waveform.
// ---------------------------------------------------------------------------

class AudioWorkspace::Track
{
public:
    Track(std::string path, Uint32 asyncEvent)
        : path_(std::move(path)), event_(asyncEvent)
    {
        decoder_ = AudioDecoder::open(path_, error_);
        if (!decoder_)
            return;
        channels_ = decoder_->channels();
        rate_ = decoder_->sampleRate();
        frames_ = decoder_->frames();
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            error_ = std::string("Audio output isn't available:\n") + SDL_GetError();
            decoder_.reset();
            return;
        }
        audioInit_ = true;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, channels_, rate_};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Track::feed, this);
        if (!stream_) {
            error_ = std::string("Could not open the audio device:\n") + SDL_GetError();
            decoder_.reset();
            return;
        }
        // The waveform: a second decoder, so playback never waits on it.
        worker_ = std::thread([this] { scan(); });
    }

    ~Track()
    {
        stopping_ = true;
        if (worker_.joinable())
            worker_.join();
        if (stream_)
            SDL_DestroyAudioStream(stream_); // stops the device; no callback after this returns
        if (audioInit_)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    const std::string& path() const { return path_; }
    const std::string& error() const { return error_; }
    bool ok() const { return decoder_ != nullptr; }
    bool playing() const { return playing_; }
    int channels() const { return channels_; }
    int rate() const { return rate_; }
    const char* format() const { return decoder_ ? decoder_->format() : ""; }
    float speed() const { return kSpeeds[speed_]; }
    float volume() const { return volume_; }

    uint64_t totalFrames() const
    {
        if (frames_ > 0)
            return frames_;
        return scanned_ ? scannedFrames_.load() : 0;
    }
    double duration() const { return rate_ > 0 ? static_cast<double>(totalFrames()) / rate_ : 0.0; }

    // The frame being heard: what was decoded minus what is still queued.
    uint64_t position()
    {
        if (!stream_)
            return 0;
        SDL_LockAudioStream(stream_);
        const int queued = SDL_GetAudioStreamQueued(stream_);
        const uint64_t frames = queued > 0 ? static_cast<uint64_t>(queued) / bytesPerFrame() : 0;
        const uint64_t at = decoded_ > frames ? decoded_ - frames : 0;
        SDL_UnlockAudioStream(stream_);
        return at;
    }

    void play()
    {
        if (!stream_ || playing_)
            return;
        if (finished())
            seek(0); // play again from the start
        SDL_ResumeAudioStreamDevice(stream_);
        playing_ = true;
    }

    void pause()
    {
        if (!stream_ || !playing_)
            return;
        SDL_PauseAudioStreamDevice(stream_);
        playing_ = false;
    }

    void seek(uint64_t frame)
    {
        if (!stream_)
            return;
        const uint64_t total = totalFrames();
        if (total > 0)
            frame = std::min(frame, total);
        SDL_LockAudioStream(stream_);
        decoder_->seek(frame);
        SDL_ClearAudioStream(stream_);
        decoded_ = frame;
        ended_ = false;
        SDL_UnlockAudioStream(stream_);
    }

    void seekSeconds(double seconds) { seek(static_cast<uint64_t>(std::max(0.0, seconds) * rate_)); }

    void stepSpeed(int direction)
    {
        const int next = std::clamp(static_cast<int>(speed_) + direction, 0, static_cast<int>(std::size(kSpeeds)) - 1);
        speed_ = static_cast<size_t>(next);
        if (stream_)
            SDL_SetAudioStreamFrequencyRatio(stream_, kSpeeds[speed_]);
    }

    void cycleSpeed()
    {
        speed_ = (speed_ + 1) % std::size(kSpeeds);
        if (stream_)
            SDL_SetAudioStreamFrequencyRatio(stream_, kSpeeds[speed_]);
    }

    void stepVolume(float delta)
    {
        volume_ = std::clamp(std::round((volume_ + delta) * 10.0f) / 10.0f, 0.0f, 1.5f);
        if (stream_)
            SDL_SetAudioStreamGain(stream_, volume_);
    }

    // Stops at the end of the file once everything queued has been heard.
    bool checkEnd()
    {
        if (!playing_ || !ended_)
            return false;
        SDL_LockAudioStream(stream_);
        const bool drained = SDL_GetAudioStreamQueued(stream_) <= 0;
        SDL_UnlockAudioStream(stream_);
        if (!drained)
            return false;
        pause();
        return true;
    }

    bool finished() const { return ended_; }

    // Peaks so far as (min, max) pairs, one per kFramesPerPeak frames.
    template <typename Fn>
    void withPeaks(Fn&& fn)
    {
        std::lock_guard lock(peaksMutex_);
        fn(peaks_);
    }
    bool scanned() const { return scanned_; }

private:
    uint64_t bytesPerFrame() const { return static_cast<uint64_t>(channels_) * sizeof(float); }

    // The device wants more: decode it. Runs on SDL's audio thread with the
    // stream locked, which is what seek() takes too.
    static void SDLCALL feed(void* user, SDL_AudioStream* stream, int additional, int)
    {
        auto* self = static_cast<Track*>(user);
        if (self->ended_ || additional <= 0)
            return;
        const uint64_t want = std::max<uint64_t>(static_cast<uint64_t>(additional) / self->bytesPerFrame(), 256);
        self->buffer_.resize(static_cast<size_t>(want * static_cast<uint64_t>(self->channels_)));
        const uint64_t got = self->decoder_->read(self->buffer_.data(), want);
        if (got > 0) {
            SDL_PutAudioStreamData(stream, self->buffer_.data(), static_cast<int>(got * self->bytesPerFrame()));
            self->decoded_ += got;
        }
        if (got < want) {
            self->ended_ = true;
            SDL_FlushAudioStream(stream);
        }
    }

    // Worker: decodes the file once into peaks for the waveform.
    void scan()
    {
        std::string error;
        std::unique_ptr<AudioDecoder> decoder = AudioDecoder::open(path_, error);
        if (!decoder)
            return;
        const int channels = decoder->channels();
        std::vector<float> block(static_cast<size_t>(kFramesPerPeak) * static_cast<size_t>(channels));
        std::vector<std::pair<float, float>> batch;
        uint64_t total = 0;
        Uint64 lastNotice = SDL_GetTicks();
        while (!stopping_) {
            const uint64_t got = decoder->read(block.data(), kFramesPerPeak);
            if (got == 0)
                break;
            float lo = 0.0f, hi = 0.0f;
            const size_t n = static_cast<size_t>(got) * static_cast<size_t>(channels);
            for (size_t i = 0; i < n; ++i) {
                lo = std::min(lo, block[i]);
                hi = std::max(hi, block[i]);
            }
            batch.emplace_back(lo, hi);
            total += got;
            if (batch.size() >= 256) {
                std::lock_guard lock(peaksMutex_);
                peaks_.insert(peaks_.end(), batch.begin(), batch.end());
                batch.clear();
            }
            if (SDL_GetTicks() - lastNotice > 250) { // let the waveform fill in as it goes
                lastNotice = SDL_GetTicks();
                notify();
            }
            if (got < kFramesPerPeak)
                break;
        }
        {
            std::lock_guard lock(peaksMutex_);
            peaks_.insert(peaks_.end(), batch.begin(), batch.end());
        }
        scannedFrames_ = total;
        scanned_ = true;
        notify();
    }

    void notify() const
    {
        SDL_Event event{};
        event.type = event_;
        SDL_PushEvent(&event);
    }

    std::string path_;
    Uint32 event_;
    std::string error_;
    std::unique_ptr<AudioDecoder> decoder_;
    int channels_ = 0;
    int rate_ = 0;
    uint64_t frames_ = 0;
    bool audioInit_ = false;
    SDL_AudioStream* stream_ = nullptr;
    bool playing_ = false;
    size_t speed_ = kNormalSpeed;
    float volume_ = 1.0f;

    // Shared with the audio thread (under the stream's lock)
    uint64_t decoded_ = 0;
    bool ended_ = false;
    std::vector<float> buffer_;

    // Shared with the waveform worker
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> scanned_{false};
    std::atomic<uint64_t> scannedFrames_{0};
    std::mutex peaksMutex_;
    std::vector<std::pair<float, float>> peaks_;
};

// ---------------------------------------------------------------------------
// The workspace
// ---------------------------------------------------------------------------

AudioWorkspace::AudioWorkspace(const WorkspaceContext& context)
    : Workspace(context)
{
}

AudioWorkspace::~AudioWorkspace() = default;

bool AudioWorkspace::openTarget(const std::string& path)
{
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->path() == path) {
            selectTab(i);
            return true;
        }
    }
    if (!tabs_.empty())
        tabs_[active_]->pause();
    const size_t at = tabs_.empty() ? 0 : active_ + 1;
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::make_unique<Track>(path, ctx_.asyncEvent));
    active_ = at;
    layout();
    return true;
}

void AudioWorkspace::setBounds(const SDL_FRect& bounds)
{
    bounds_ = bounds;
    layout();
}

void AudioWorkspace::layout()
{
    std::vector<Toolbar::Item> items;
    if (!tabs_.empty() && tabs_[active_]->ok()) {
        Track& t = *tabs_[active_];
        const double now = t.rate() > 0 ? static_cast<double>(t.position()) / t.rate() : 0.0;
        items.push_back({kToolPlay, "", t.playing() ? Toolbar::Icon::Pause : Toolbar::Icon::Play});
        items.push_back({0, clock(now) + " / " + (t.duration() > 0.0 ? clock(t.duration()) : "--:--")});
        items.push_back({kToolSpeed, speedLabel(t.speed()), Toolbar::Icon::None, true, true});
        items.push_back({kToolVolumeDown, "", Toolbar::Icon::Minus, t.volume() > 0.0f, true});
        items.push_back({0, std::to_string(static_cast<int>(std::lround(t.volume() * 100.0f))) + "%",
                         Toolbar::Icon::None, true, true});
        items.push_back({kToolVolumeUp, "", Toolbar::Icon::Plus, t.volume() < 1.5f, true});
    }
    toolbar_.set({bounds_.x, bounds_.y, bounds_.w, Toolbar::heightFor(*ctx_.code)}, *ctx_.code, std::move(items));
}

SDL_FRect AudioWorkspace::waveRect() const
{
    const float bar = Toolbar::heightFor(*ctx_.code);
    const float lh = static_cast<float>(ctx_.code->font().lineHeight());
    const float pad = std::round(lh);
    const float top = bounds_.y + bar + pad * 2.5f; // room for the file's details
    const float h = std::max(0.0f, std::min(bounds_.y + bounds_.h - pad - top, std::round(lh * 10.0f)));
    return {bounds_.x + pad, top, std::max(0.0f, bounds_.w - 2.0f * pad), h};
}

void AudioWorkspace::seekTo(float x)
{
    if (tabs_.empty() || !tabs_[active_]->ok())
        return;
    Track& t = *tabs_[active_];
    const SDL_FRect wave = waveRect();
    if (wave.w <= 0.0f)
        return;
    const double f = std::clamp(static_cast<double>((x - wave.x) / wave.w), 0.0, 1.0);
    t.seek(static_cast<uint64_t>(f * static_cast<double>(t.totalFrames())));
}

bool AudioWorkspace::handleEvent(const SDL_Event& event)
{
    if (tabs_.empty() || !tabs_[active_]->ok())
        return false;
    Track& t = *tabs_[active_];
    layout();
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (scrubbing_) {
            seekTo(event.motion.x);
            return true;
        }
        return toolbar_.hover(event.motion.x, event.motion.y);
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (event.button.button != SDL_BUTTON_LEFT)
            return false;
        switch (toolbar_.hit(event.button.x, event.button.y)) {
        case kToolPlay:
            t.playing() ? t.pause() : t.play();
            return true;
        case kToolSpeed:
            t.cycleSpeed();
            return true;
        case kToolVolumeDown:
            t.stepVolume(-0.1f);
            return true;
        case kToolVolumeUp:
            t.stepVolume(0.1f);
            return true;
        default:
            break;
        }
        const SDL_FRect wave = waveRect();
        const SDL_FRect grab{wave.x - 4.0f, wave.y, wave.w + 8.0f, wave.h};
        if (ui::contains(grab, event.button.x, event.button.y) && t.totalFrames() > 0) {
            scrubbing_ = true;
            seekTo(event.button.x);
            return true;
        }
        return false;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (scrubbing_ && event.button.button == SDL_BUTTON_LEFT) {
            scrubbing_ = false;
            return true;
        }
        return false;
    case SDL_EVENT_KEY_DOWN: {
        const double now = static_cast<double>(t.position()) / std::max(1, t.rate());
        switch (event.key.key) {
        case SDLK_SPACE:
        case SDLK_K:
            t.playing() ? t.pause() : t.play();
            return true;
        case SDLK_LEFT:
        case SDLK_J:
            t.seekSeconds(now - kSeekSeconds);
            return true;
        case SDLK_RIGHT:
        case SDLK_L:
            t.seekSeconds(now + kSeekSeconds);
            return true;
        case SDLK_HOME:
            t.seek(0);
            return true;
        case SDLK_END:
            t.seek(t.totalFrames());
            return true;
        case SDLK_LEFTBRACKET:
            t.stepSpeed(-1);
            return true;
        case SDLK_RIGHTBRACKET:
            t.stepSpeed(1);
            return true;
        case SDLK_UP:
            t.stepVolume(0.1f);
            return true;
        case SDLK_DOWN:
            t.stepVolume(-0.1f);
            return true;
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

void AudioWorkspace::draw(SDL_Renderer* renderer)
{
    ui::fillRect(renderer, bounds_, theme::kEditorBg);
    TextRenderer& text = *ctx_.code;
    if (tabs_.empty()) {
        ui::drawMessage(renderer, text, bounds_,
                        {"No audio open", "Open a .wav, .ogg, .mp3 or .flac file with File > Open,",
                         "click a link to one, or drop it here."});
        return;
    }
    Track& t = *tabs_[active_];
    if (!t.ok()) {
        std::vector<std::string> lines = {"Could not play " + embed::fileName(t.path())};
        size_t start = 0;
        const std::string& e = t.error();
        while (start <= e.size()) {
            const size_t end = std::min(e.find('\n', start), e.size());
            lines.push_back(e.substr(start, end - start));
            start = end + 1;
        }
        ui::drawMessage(renderer, text, bounds_, lines);
        return;
    }
    t.checkEnd();
    layout();
    toolbar_.draw(renderer, text);

    // Details above the waveform
    const SDL_FRect wave = waveRect();
    const float lh = static_cast<float>(text.font().lineHeight());
    char details[160];
    SDL_snprintf(details, sizeof(details), "%s  \xC2\xB7  %.1f kHz  \xC2\xB7  %s", t.format(), t.rate() / 1000.0,
                 t.channels() == 1 ? "Mono" : t.channels() == 2 ? "Stereo" : (std::to_string(t.channels()) + " channels").c_str());
    text.draw(wave.x, std::round(wave.y - lh * 1.8f), embed::fileName(t.path()), theme::kMenuText, wave.x,
              wave.x + wave.w);
    text.draw(std::round(wave.x + text.measure(embed::fileName(t.path())) + lh), std::round(wave.y - lh * 1.8f),
              details, theme::kMuted, wave.x, wave.x + wave.w);
    text.flush();
    if (wave.w <= 0.0f || wave.h <= 0.0f)
        return;

    ui::fillRect(renderer, wave, theme::kPanelBg);
    const float mid = std::round(wave.y + wave.h * 0.5f);
    const uint64_t total = t.totalFrames();
    const uint64_t at = t.position();
    const float playX = total > 0 ? wave.x + wave.w * static_cast<float>(static_cast<double>(at) / total) : wave.x;

    // One bar per pixel column, from the peaks under it.
    std::vector<SDL_FRect> played, ahead;
    t.withPeaks([&](const std::vector<std::pair<float, float>>& peaks) {
        if (peaks.empty() || total == 0)
            return;
        const double perColumn = static_cast<double>(total) / kFramesPerPeak / wave.w;
        const int columns = static_cast<int>(wave.w);
        played.reserve(static_cast<size_t>(columns));
        ahead.reserve(static_cast<size_t>(columns));
        for (int c = 0; c < columns; ++c) {
            size_t a = static_cast<size_t>(c * perColumn);
            size_t b = std::max(a + 1, static_cast<size_t>((c + 1) * perColumn));
            if (a >= peaks.size())
                break;
            b = std::min(b, peaks.size());
            float lo = 0.0f, hi = 0.0f;
            for (size_t i = a; i < b; ++i) {
                lo = std::min(lo, peaks[i].first);
                hi = std::max(hi, peaks[i].second);
            }
            const float top = mid - std::min(1.0f, hi) * wave.h * 0.48f;
            const float bottom = mid - std::max(-1.0f, lo) * wave.h * 0.48f;
            const float x = wave.x + static_cast<float>(c);
            (x < playX ? played : ahead).push_back({x, std::floor(top), 1.0f, std::max(1.0f, std::ceil(bottom - top))});
        }
    });
    ui::fillRect(renderer, {wave.x, mid, wave.w, 1.0f}, theme::kBorder);
    SDL_SetRenderDrawColorFloat(renderer, theme::kMenuActive.r, theme::kMenuActive.g, theme::kMenuActive.b, 1.0f);
    SDL_RenderFillRects(renderer, played.data(), static_cast<int>(played.size()));
    SDL_SetRenderDrawColorFloat(renderer, theme::kGutterText.r, theme::kGutterText.g, theme::kGutterText.b, 1.0f);
    SDL_RenderFillRects(renderer, ahead.data(), static_cast<int>(ahead.size()));
    if (!t.scanned()) {
        const std::string note = "Reading the waveform...";
        text.draw(std::round(wave.x + wave.w - text.measure(note) - lh * 0.5f), wave.y + lh * 0.3f, note,
                  theme::kMuted);
        text.flush();
    }
    // The playhead
    ui::fillRect(renderer, {std::round(playX) - 1.0f, wave.y, 2.0f, wave.h}, theme::kText);
    ui::strokeRect(renderer, wave, theme::kBorder);

    // Keys, under the waveform
    const std::string help = "Space play/pause  \xC2\xB7  \xE2\x86\x90 \xE2\x86\x92 5 s  \xC2\xB7  [ ] speed  \xC2\xB7  "
                             "\xE2\x86\x91 \xE2\x86\x93 volume";
    if (wave.y + wave.h + lh * 2.0f < bounds_.y + bounds_.h)
        text.draw(wave.x, std::round(wave.y + wave.h + lh * 0.8f), help, theme::kGutterText, wave.x,
                  wave.x + wave.w);
    text.flush();
}

Workspace::Pointer AudioWorkspace::pointerAt(float x, float y)
{
    if (toolbar_.hit(x, y))
        return Pointer::Hand;
    if (!tabs_.empty() && tabs_[active_]->ok() && ui::contains(waveRect(), x, y))
        return Pointer::Hand;
    return Pointer::Arrow;
}

std::string AudioWorkspace::status()
{
    if (tabs_.empty() || !tabs_[active_]->ok())
        return "Audio";
    Track& t = *tabs_[active_];
    return std::string(t.playing() ? "Playing" : "Paused") + " \xC2\xB7 " + t.format();
}

void AudioWorkspace::selectTab(size_t index)
{
    if (index >= tabs_.size() || index == active_)
        return;
    tabs_[active_]->pause(); // only the shown tab plays
    active_ = index;
    scrubbing_ = false;
    layout();
}

std::string AudioWorkspace::tabTitle(size_t index) const
{
    return index < tabs_.size() ? embed::fileName(tabs_[index]->path()) : "Audio";
}

void AudioWorkspace::closeTab(size_t index)
{
    if (index >= tabs_.size())
        return;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (active_ >= tabs_.size())
        active_ = tabs_.empty() ? 0 : tabs_.size() - 1;
    else if (index < active_)
        --active_;
    scrubbing_ = false;
    layout();
}

bool AudioWorkspace::onAsync()
{
    // Waveform progress: only the shown tab draws it.
    return !tabs_.empty();
}

Sint32 AudioWorkspace::wakeMs() const
{
    return !tabs_.empty() && tabs_[active_]->playing() ? 33 : -1;
}

void AudioWorkspace::setVisibility(bool shown, bool)
{
    if (!shown && !tabs_.empty())
        tabs_[active_]->pause();
}
