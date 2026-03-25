#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace atomicripper::drive {

// One track entry from the disc Table of Contents
struct TrackInfo {
    int      number;       // 1-based track number
    bool     isAudio;      // false = data track (e.g. CD-ROM)
    uint32_t lba;          // start sector, LBA (Logical Block Address)
    uint32_t sectorCount;  // number of 2352-byte sectors in this track
    std::string isrc;      // ISRC code, may be empty
};

// Full Table of Contents for an inserted disc
struct TOC {
    int      firstTrack  = 0;
    int      lastTrack   = 0;
    uint32_t leadOutLBA  = 0;  // LBA of the lead-out (= total sector count)

    std::vector<TrackInfo> tracks;

    // Convenience
    int     audioTrackCount() const;
    double  durationSeconds()  const { return static_cast<double>(leadOutLBA) / 75.0; }
    bool    isValid()          const { return firstTrack > 0 && !tracks.empty(); }
};

} // namespace atomicripper::drive
