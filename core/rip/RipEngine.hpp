#pragma once
#include "../drive/TOC.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace atomicripper::rip {

// ---------------------------------------------------------------------------
// Rip quality modes
// ---------------------------------------------------------------------------
enum class RipMode {
    Burst,    // single pass — fast, no verification
    Secure,   // multi-pass — default: re-reads mismatching sectors
    Paranoia  // same algorithm as Secure for now; will get jitter correction in Phase 3
};

// ---------------------------------------------------------------------------
// Per-sector quality result (also carries the raw audio bytes internally)
// ---------------------------------------------------------------------------
struct SectorResult {
    uint32_t             lba;
    std::vector<uint8_t> data;        // 2352 bytes of raw CD-DA audio
    uint8_t              confidence;  // how many reads agreed (0 = no consensus)
    bool                 hasC2Errors; // drive reported bit errors in this sector
    int                  retries;     // extra reads needed beyond the initial passes
};

// ---------------------------------------------------------------------------
// Progress snapshot — emitted roughly every 10 sectors during a rip
// ---------------------------------------------------------------------------
struct RipProgress {
    int      currentTrack;
    uint32_t currentSector;   // sectors completed so far in this track
    uint32_t totalSectors;    // total sectors in this track
    float    speedX;          // rip speed relative to 1× (150 KB/s)
    int      totalRetries;    // cumulative retries so far
};

// ---------------------------------------------------------------------------
// Settings for a rip job
// ---------------------------------------------------------------------------
struct RipSettings {
    RipMode mode         = RipMode::Secure;
    int     maxRetries   = 16;   // max extra reads per sector before giving up
    int     minMatches   = 2;    // identical reads required before accepting a sector
    bool    useC2Errors  = true; // use drive C2 error pointers if supported
    int     driveOffset  = 0;    // read offset correction in samples (+/-)
};

// ---------------------------------------------------------------------------
// Result returned from ripTrack()
// ---------------------------------------------------------------------------
struct TrackRipResult {
    std::vector<SectorResult> sectors;     // per-sector quality info
    std::vector<uint8_t>      data;        // offset-corrected PCM: 2352 * sectorCount bytes
    uint32_t                  crc32    = 0;
    bool                      ok       = false;
    bool                      cancelled = false;
};

// ---------------------------------------------------------------------------
// RipEngine — Phase 2 implementation
// ---------------------------------------------------------------------------
class RipEngine {
public:
    using ProgressCallback = std::function<void(const RipProgress&)>;

    explicit RipEngine(std::string drivePath, RipSettings settings = {});
    ~RipEngine();

    // Open/close the drive handle
    bool open();
    void close();
    bool isOpen() const;

    // Probe whether the drive supports C2 error reporting.
    // Call once after open() before ripping.
    bool probeCapabilities();
    bool supportsC2() const { return m_supportsC2; }

    // Rip a single track. Blocks until complete or cancelled.
    // onProgress is called roughly every 10 sectors.
    TrackRipResult ripTrack(const drive::TrackInfo& track,
                            int                    trackNumberHint = 1,
                            ProgressCallback       onProgress      = nullptr);

    void cancel()             { m_cancelled = true; }
    bool isCancelled() const  { return m_cancelled.load(); }

private:
    std::string       m_drivePath;
    RipSettings       m_settings;
    bool              m_supportsC2 = false;
    std::atomic<bool> m_cancelled{false};

    // Platform private helpers (defined per-platform in RipEngine.cpp)
    SectorResult ripSectorBurst(uint32_t lba);
    SectorResult ripSectorSecure(uint32_t lba);

#ifdef _WIN32
    // Stored as void* so windows.h stays out of this header.
    // Invariant: nullptr == not open, (void*)-1 == INVALID_HANDLE_VALUE
    void* m_handle = nullptr;

    // Send a READ CD (0xBE) SCSI command via SPTI.
    // outBuf must be at least count * (withC2 ? 2646 : 2352) bytes.
    bool readSectors(uint32_t lba, int count, uint8_t* outBuf, bool withC2);
#endif
};

} // namespace atomicripper::rip
