#include "audio/AudioDecoder.h"

#include <SDL3/SDL.h>

#include <algorithm>

#define DR_WAV_NO_STDIO
#define DR_MP3_NO_STDIO
#define DR_FLAC_NO_STDIO
#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

namespace {

std::string extensionOf(std::string_view path)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    std::string ext(path.substr(dot + 1));
    for (char& c : ext)
        c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
    return ext;
}

// dr_libs read through SDL, so paths are UTF-8 everywhere.
struct File
{
    SDL_IOStream* io = nullptr;

    ~File()
    {
        if (io)
            SDL_CloseIO(io);
    }

    static size_t read(void* user, void* out, size_t bytes)
    {
        return SDL_ReadIO(static_cast<File*>(user)->io, out, bytes);
    }

    static SDL_IOWhence whence(int origin) // the three libraries share set/cur/end as 0/1/2
    {
        return origin == 0 ? SDL_IO_SEEK_SET : origin == 1 ? SDL_IO_SEEK_CUR : SDL_IO_SEEK_END;
    }

    static bool seek(void* user, int offset, int origin)
    {
        return SDL_SeekIO(static_cast<File*>(user)->io, offset, whence(origin)) >= 0;
    }

    static bool tell(void* user, long long* cursor) // the libraries' int64 is long long everywhere
    {
        const Sint64 at = SDL_TellIO(static_cast<File*>(user)->io);
        *cursor = at;
        return at >= 0;
    }
};

class WavDecoder final : public AudioDecoder
{
public:
    bool open(const std::string& path)
    {
        file_.io = SDL_IOFromFile(path.c_str(), "rb");
        if (!file_.io)
            return false;
        const auto onSeek = [](void* u, int o, drwav_seek_origin w) -> drwav_bool32 {
            return File::seek(u, o, static_cast<int>(w));
        };
        const auto onTell = [](void* u, drwav_int64* c) -> drwav_bool32 { return File::tell(u, c); };
        if (!drwav_init(&wav_, &File::read, onSeek, onTell, &file_, nullptr))
            return false;
        ready_ = true;
        channels_ = wav_.channels;
        sampleRate_ = static_cast<int>(wav_.sampleRate);
        frames_ = wav_.totalPCMFrameCount;
        format_ = "WAV";
        return true;
    }
    ~WavDecoder() override
    {
        if (ready_)
            drwav_uninit(&wav_);
    }
    uint64_t read(float* out, uint64_t count) override { return drwav_read_pcm_frames_f32(&wav_, count, out); }
    bool seek(uint64_t frame) override { return drwav_seek_to_pcm_frame(&wav_, frame); }

private:
    File file_;
    drwav wav_{};
    bool ready_ = false;
};

class Mp3Decoder final : public AudioDecoder
{
public:
    bool open(const std::string& path)
    {
        file_.io = SDL_IOFromFile(path.c_str(), "rb");
        if (!file_.io)
            return false;
        const auto onSeek = [](void* u, int o, drmp3_seek_origin w) -> drmp3_bool32 {
            return File::seek(u, o, static_cast<int>(w));
        };
        const auto onTell = [](void* u, drmp3_int64* c) -> drmp3_bool32 { return File::tell(u, c); };
        if (!drmp3_init(&mp3_, &File::read, onSeek, onTell, nullptr, &file_, nullptr))
            return false;
        ready_ = true;
        channels_ = static_cast<int>(mp3_.channels);
        sampleRate_ = static_cast<int>(mp3_.sampleRate);
        format_ = "MP3";
        return true;
    }
    ~Mp3Decoder() override
    {
        if (ready_)
            drmp3_uninit(&mp3_);
    }
    uint64_t read(float* out, uint64_t count) override { return drmp3_read_pcm_frames_f32(&mp3_, count, out); }
    bool seek(uint64_t frame) override { return drmp3_seek_to_pcm_frame(&mp3_, frame); }

private:
    File file_;
    drmp3 mp3_{};
    bool ready_ = false;
};

class FlacDecoder final : public AudioDecoder
{
public:
    bool open(const std::string& path)
    {
        file_.io = SDL_IOFromFile(path.c_str(), "rb");
        if (!file_.io)
            return false;
        const auto onSeek = [](void* u, int o, drflac_seek_origin w) -> drflac_bool32 {
            return File::seek(u, o, static_cast<int>(w));
        };
        const auto onTell = [](void* u, drflac_int64* c) -> drflac_bool32 { return File::tell(u, c); };
        flac_ = drflac_open(&File::read, onSeek, onTell, &file_, nullptr);
        if (!flac_)
            return false;
        channels_ = flac_->channels;
        sampleRate_ = static_cast<int>(flac_->sampleRate);
        frames_ = flac_->totalPCMFrameCount;
        format_ = "FLAC";
        return true;
    }
    ~FlacDecoder() override { drflac_close(flac_); }
    uint64_t read(float* out, uint64_t count) override { return drflac_read_pcm_frames_f32(flac_, count, out); }
    bool seek(uint64_t frame) override { return drflac_seek_to_pcm_frame(flac_, frame); }

private:
    File file_;
    drflac* flac_ = nullptr;
};

// stb_vorbis reads from memory; Ogg files are compressed, so that's small.
class VorbisDecoder final : public AudioDecoder
{
public:
    bool open(const std::string& path)
    {
        size_t size = 0;
        data_ = static_cast<unsigned char*>(SDL_LoadFile(path.c_str(), &size));
        if (!data_ || size > 0x7FFFFFFF)
            return false;
        int error = 0;
        vorbis_ = stb_vorbis_open_memory(data_, static_cast<int>(size), &error, nullptr);
        if (!vorbis_)
            return false;
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
        channels_ = info.channels;
        sampleRate_ = static_cast<int>(info.sample_rate);
        frames_ = stb_vorbis_stream_length_in_samples(vorbis_);
        format_ = "Ogg Vorbis";
        return true;
    }
    ~VorbisDecoder() override
    {
        if (vorbis_)
            stb_vorbis_close(vorbis_);
        SDL_free(data_);
    }
    uint64_t read(float* out, uint64_t count) override
    {
        const int floats = static_cast<int>(std::min<uint64_t>(count, 1u << 20) * static_cast<uint64_t>(channels_));
        return static_cast<uint64_t>(stb_vorbis_get_samples_float_interleaved(vorbis_, channels_, out, floats));
    }
    bool seek(uint64_t frame) override { return stb_vorbis_seek(vorbis_, static_cast<unsigned int>(frame)) != 0; }

private:
    unsigned char* data_ = nullptr;
    stb_vorbis* vorbis_ = nullptr;
};

template <typename T>
std::unique_ptr<AudioDecoder> tryOpen(const std::string& path)
{
    auto decoder = std::make_unique<T>();
    if (!decoder->open(path) || decoder->channels() <= 0 || decoder->sampleRate() <= 0)
        return nullptr;
    return decoder;
}

} // namespace

bool AudioDecoder::handles(std::string_view path)
{
    const std::string ext = extensionOf(path);
    return ext == "wav" || ext == "wave" || ext == "ogg" || ext == "oga" || ext == "mp3" || ext == "flac";
}

std::unique_ptr<AudioDecoder> AudioDecoder::open(const std::string& path, std::string& error)
{
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(path.c_str(), &info)) {
        error = "The file doesn't exist.";
        return nullptr;
    }
    const std::string ext = extensionOf(path);
    std::unique_ptr<AudioDecoder> decoder;
    // The extension first; then everything, for misnamed files.
    if (ext == "wav" || ext == "wave")
        decoder = tryOpen<WavDecoder>(path);
    else if (ext == "ogg" || ext == "oga")
        decoder = tryOpen<VorbisDecoder>(path);
    else if (ext == "mp3")
        decoder = tryOpen<Mp3Decoder>(path);
    else if (ext == "flac")
        decoder = tryOpen<FlacDecoder>(path);
    if (!decoder)
        decoder = tryOpen<WavDecoder>(path);
    if (!decoder)
        decoder = tryOpen<FlacDecoder>(path);
    if (!decoder)
        decoder = tryOpen<VorbisDecoder>(path);
    if (!decoder && ext == "mp3")
        decoder = tryOpen<Mp3Decoder>(path); // MP3 has no reliable signature: only when named so
    if (!decoder) {
        error = ext == "ogg" || ext == "oga"
                    ? "This Ogg file isn't Vorbis audio (Opus isn't supported yet), or it is damaged."
                    : "This file's audio format isn't supported, or the file is damaged.";
    }
    return decoder;
}
