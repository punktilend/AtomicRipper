#pragma once
#include "IEncoder.hpp"
#include <cstdint>
#include <cstdio>
#include <string>

namespace atomicripper::encode {

// ---------------------------------------------------------------------------
// WavEncoder — uncompressed PCM RIFF/WAV output
//
// Writes a standard 44-byte WAV header (RIFF/fmt /data chunks).
// If totalSamples is set in EncoderSettings the sizes are written correctly
// in open(); otherwise placeholders are used and finalize() seeks back to
// patch the RIFF and data chunk sizes.
//
// WAV has no standard tag support; setTag() is accepted but ignored.
//
// Typical usage (identical to FlacEncoder):
//   WavEncoder enc;
//   enc.open("track01.wav", settings);
//   enc.writeSamples(samples);
//   enc.finalize();
// ---------------------------------------------------------------------------
class WavEncoder : public IEncoder {
public:
    WavEncoder();
    ~WavEncoder() override;

    // Tags are silently ignored — WAV has no standard metadata block.
    void setTag(const std::string& key, const std::string& value);

    // IEncoder interface
    bool        open(const std::filesystem::path& outputPath,
                     const EncoderSettings& settings) override;
    bool        writeSamples(std::span<const int32_t> samples) override;
    bool        finalize() override;
    const char* name() const override { return "WAV"; }

    std::string lastError() const;

private:
    FILE*       m_fp        = nullptr;
    uint32_t    m_dataBytes = 0;      // running total of PCM bytes written
    int         m_channels  = 2;
    int         m_bitsPerSample = 16;
    bool        m_opened    = false;
    bool        m_finalized = false;
    std::string m_lastError;

    // Write RIFF + fmt + data chunk headers at current file position.
    // dataSize = 0 means "unknown" (placeholder).
    void writeHeaders(uint32_t dataSize);
};

} // namespace atomicripper::encode
