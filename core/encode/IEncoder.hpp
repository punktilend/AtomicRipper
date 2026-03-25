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
    ALAC,  // Apple Lossless — best for Apple ecosystem (Phase 4+)
};

// ---------------------------------------------------------------------------
// Per-encode settings
// ---------------------------------------------------------------------------
struct EncoderSettings {
    Format   format           = Format::FLAC;
    int      compressionLevel = 8;       // FLAC: 0 (fastest) – 8 (smallest/slowest)
    int      sampleRate       = 44100;   // Hz — always 44100 for standard CD
    int      channels         = 2;       // always 2 for CD audio
    int      bitsPerSample    = 16;      // always 16 for standard CD
    uint64_t totalSamples     = 0;       // 0 = unknown; set for accurate seek tables

    bool     embeddedCueSheet    = false; // write FLAC CUESHEET block (Phase 4)
    bool     calculateReplayGain = false; // compute & tag track/album gain (Phase 5)
};

// ---------------------------------------------------------------------------
// IEncoder — base interface for all output format encoders
// ---------------------------------------------------------------------------
class IEncoder {
public:
    virtual ~IEncoder() = default;

    // Open output file and begin encoding stream.
    // Tags should be set on the concrete encoder before calling open().
    virtual bool open(const std::filesystem::path& outputPath,
                      const EncoderSettings& settings) = 0;

    // Feed interleaved signed 32-bit PCM samples (L R L R …).
    // For 16-bit CD audio, values are in the range [-32768, 32767].
    virtual bool writeSamples(std::span<const int32_t> samples) = 0;

    // Flush, finalise metadata, and close the file. Must be called exactly once.
    virtual bool finalize() = 0;

    // Human-readable name of this encoder (e.g. "FLAC", "WAV")
    virtual const char* name() const = 0;
};

} // namespace atomicripper::encode
