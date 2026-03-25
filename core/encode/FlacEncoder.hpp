#pragma once
#include "IEncoder.hpp"
#include <memory>
#include <string>

namespace atomicripper::encode {

// ---------------------------------------------------------------------------
// FlacEncoder — lossless FLAC output via libFLAC
//
// Typical usage:
//   FlacEncoder enc;
//   enc.setTag("TRACKNUMBER", "1");
//   enc.setTag("TITLE",       "Some Song");
//   enc.open("track01.flac", settings);
//   enc.writeSamples(samples);   // call as many times as needed
//   enc.finalize();
// ---------------------------------------------------------------------------
class FlacEncoder : public IEncoder {
public:
    FlacEncoder();
    ~FlacEncoder() override;

    // Set a Vorbis Comment tag (e.g. "TITLE", "ARTIST", "TRACKNUMBER").
    // Must be called before open(). Keys are case-insensitive per spec;
    // we store them uppercase for consistency.
    void setTag(const std::string& key, const std::string& value);
    void clearTags();

    // IEncoder interface
    bool        open(const std::filesystem::path& outputPath,
                     const EncoderSettings& settings) override;
    bool        writeSamples(std::span<const int32_t> samples) override;
    bool        finalize() override;
    const char* name() const override { return "FLAC"; }

    // Human-readable description of the last error (empty if none)
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// Utility: convert raw CD-DA bytes → interleaved int32_t PCM samples.
// Input:  little-endian signed 16-bit stereo PCM (2352 bytes per sector).
// Output: int32_t values in range [-32768, 32767], L R L R interleaved.
// ---------------------------------------------------------------------------
void cdBytesToSamples(const uint8_t* bytes, size_t byteCount,
                      std::vector<int32_t>& outSamples);

} // namespace atomicripper::encode
