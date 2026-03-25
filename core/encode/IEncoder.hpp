#pragma once
#include <cstdint>
#include <filesystem>
#include <span>

namespace atomicripper::encode {

// ---------------------------------------------------------------------------
// Output format selector
// ---------------------------------------------------------------------------
enum class Format {
    FLAC,  // default — lossless, open, universally supported
    WAV,   // uncompressed PCM — maximum compatibility
    ALAC,  // Apple Lossless — best for Apple ecosystem
};

// ---------------------------------------------------------------------------
// Per-encode settings
// ---------------------------------------------------------------------------
struct EncoderSettings {
    Format format           = Format::FLAC;
    int    compressionLevel = 8;       // FLAC: 0 (fastest) – 8 (smallest)
    int    sampleRate       = 44100;   // Hz — always 44100 for standard CD
    int    channels         = 2;       // always 2 for CD audio
    int    bitsPerSample    = 16;      // always 16 for standard CD

    bool   embeddedCueSheet    = true; // write FLAC CUESHEET metadata block
    bool   calculateReplayGain = true; // compute & tag track + album gain
};

// ---------------------------------------------------------------------------
// IEncoder — implemented per-format in Phase 3
// ---------------------------------------------------------------------------
class IEncoder {
public:
    virtual ~IEncoder() = default;

    // Open output file and begin encoding stream
    virtual bool open(const std::filesystem::path& outputPath,
                      const EncoderSettings& settings) = 0;

    // Feed interleaved signed 32-bit PCM samples (L R L R …)
    // For 16-bit CD audio, values are in the range [-32768, 32767]
    virtual bool writeSamples(std::span<const int32_t> samples) = 0;

    // Flush, write metadata, and close the file
    virtual bool finalize() = 0;

    // Human-readable name of this encoder
    virtual const char* name() const = 0;
};

} // namespace atomicripper::encode
