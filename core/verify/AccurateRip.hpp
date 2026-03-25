#pragma once
#include "../drive/TOC.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace atomicripper::verify {

// ---------------------------------------------------------------------------
// Per-track result after querying the AccurateRip database
// ---------------------------------------------------------------------------
struct ArTrackResult {
    int      trackNumber  = 0;
    uint32_t checksumV1   = 0;   // our calculated v1 CRC (offset=0)
    uint32_t checksumV2   = 0;   // our calculated v2 CRC (offset=0)
    int      confidenceV1 = 0;   // how many DB submissions match our v1 CRC
    int      confidenceV2 = 0;   // how many DB submissions match our v2 CRC
    bool     matched      = false;
};

// ---------------------------------------------------------------------------
// Full disc verification result
// ---------------------------------------------------------------------------
struct ArDiscResult {
    std::vector<ArTrackResult> tracks;
    bool        lookupOk     = false;  // HTTP fetch + parse succeeded
    int         dbEntries    = 0;      // number of distinct pressings in DB
    std::string url;                   // the URL we queried (useful for logging)
    std::string error;                 // non-empty on failure
};

// ---------------------------------------------------------------------------
// AccurateRip — checksum calculation, disc ID derivation, HTTP verification
//
// Disc ID math:
//   AccurateRip uses two 32-bit IDs derived from the physical frame offsets
//   (LBA + 150, i.e. including the standard 2-second lead-in pregap):
//
//   id1 = (sum of all track frame offsets) + leadOut frame offset
//   id2 = sum(i * offset[i], i=1..N) + (N+1) * leadOut frame offset
//
//   The CDDB ID uses the same offset convention and the classic CDDB algorithm.
//
// Checksum algorithm:
//   Both versions iterate over stereo frames (one 32-bit word = L16 | R16<<16).
//   First track:  skip frames 0..2938 (2939 frames)
//   Last track:   skip last 2940 frames
//   Middle tracks: use all frames
//
//   V1:  crc += frame_value * (1-based frame index)
//   V2:  product = frame_value * (1-based frame index)
//        crc += ROR32(product, 8)   [rotate-right 8 bits]
// ---------------------------------------------------------------------------
class AccurateRip {
public:
    // Calculate AccurateRip checksums for one track's raw CD-DA PCM data.
    // pcm:     pointer to raw bytes (16-bit LE stereo, 2352 * N bytes)
    // bytes:   total byte count
    // isFirst: true if this is the first audio track on the disc
    // isLast:  true if this is the last audio track on the disc
    static uint32_t checksumV1(const uint8_t* pcm, size_t bytes,
                                bool isFirst, bool isLast);
    static uint32_t checksumV2(const uint8_t* pcm, size_t bytes,
                                bool isFirst, bool isLast);

    // Derive the three AccurateRip disc IDs from a TOC.
    static uint32_t discId1 (const drive::TOC& toc);
    static uint32_t discId2 (const drive::TOC& toc);
    static uint32_t cddbId  (const drive::TOC& toc);

    // Build the AccurateRip database URL for a disc.
    static std::string buildUrl(const drive::TOC& toc);

    // Fetch the AccurateRip DB entry over HTTP and compare against the
    // supplied track data. trackPcm must be indexed in the same order as
    // toc.tracks (audio tracks only, raw 2352-byte-per-sector PCM).
    static ArDiscResult verify(
        const drive::TOC&                        toc,
        const std::vector<std::vector<uint8_t>>& trackPcm);
};

} // namespace atomicripper::verify
