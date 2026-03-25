#pragma once
#include "../drive/TOC.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace atomicripper::rip {

// ---------------------------------------------------------------------------
// Rip quality modes
// ---------------------------------------------------------------------------
enum class RipMode {
    Burst,    // single pass — fast, no verification
    Secure,   // multi-pass — default: re-reads mismatching sectors
    Paranoia  // full jitter/scratch correction (slowest, most thorough)
};

// ---------------------------------------------------------------------------
// Per-sector result
// ---------------------------------------------------------------------------
struct SectorResult {
    uint32_t              lba;
    std::vector<uint8_t>  data;         // always 2352 bytes (raw CD audio sector)
    uint8_t               confidence;   // 0–255: how many reads agreed
    bool                  hasC2Errors;  // true = drive flagged bit errors in this sector
    int                   retries;      // how many extra reads were needed
};

// ---------------------------------------------------------------------------
// Progress snapshot emitted during a rip
// ---------------------------------------------------------------------------
struct RipProgress {
    int      currentTrack;
    uint32_t currentSector;
    uint32_t totalSectors;
    float    speedX;        // rip speed relative to 1× (150 KB/s)
    int      totalRetries;  // cumulative retry count so far
};

// ---------------------------------------------------------------------------
// Settings for a rip job
// ---------------------------------------------------------------------------
struct RipSettings {
    RipMode mode          = RipMode::Secure;
    int     maxRetries    = 16;   // max re-reads per sector before giving up
    int     minMatches    = 2;    // how many identical reads before accepting
    bool    useC2Errors   = true; // use drive C2 error pointers if supported
    int     driveOffset   = 0;    // read offset correction in samples (+/-)
};

// ---------------------------------------------------------------------------
// RipEngine — implemented in Phase 2
// ---------------------------------------------------------------------------
class RipEngine {
public:
    using ProgressCallback = std::function<void(const RipProgress&)>;
    using SectorCallback   = std::function<void(const SectorResult&)>;

    // Phase 2 will add:
    //   RipEngine(std::string drivePath, RipSettings settings);
    //   bool ripTrack(const drive::TrackInfo& track,
    //                 SectorCallback onSector,
    //                 ProgressCallback onProgress);
    //   void cancel();
};

} // namespace atomicripper::rip
