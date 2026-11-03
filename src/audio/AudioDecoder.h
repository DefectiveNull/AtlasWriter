#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// Decodes an audio file into interleaved float samples: WAV, FLAC and MP3
// through dr_libs, Ogg Vorbis through stb_vorbis. One decoder belongs to
// one thread at a time.
class AudioDecoder
{
public:
    // Whether a path has an extension the audio tile plays.
    static bool handles(std::string_view path);
    // Null on failure, with the reason in `error`.
    static std::unique_ptr<AudioDecoder> open(const std::string& path, std::string& error);

    virtual ~AudioDecoder() = default;

    int channels() const { return channels_; }
    int sampleRate() const { return sampleRate_; }
    // Total frames, or 0 when the format can't tell without decoding it all
    // (MP3); the waveform pass counts them then.
    uint64_t frames() const { return frames_; }
    const char* format() const { return format_; } // "WAV", "MP3", ...

    // Reads up to `count` frames into `out` (count * channels floats);
    // returns how many were read, 0 at the end.
    virtual uint64_t read(float* out, uint64_t count) = 0;
    virtual bool seek(uint64_t frame) = 0;

protected:
    int channels_ = 0;
    int sampleRate_ = 0;
    uint64_t frames_ = 0;
    const char* format_ = "";
};
