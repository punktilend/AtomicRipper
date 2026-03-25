#include "AccurateRip.hpp"

// Must come before any header that transitively includes windows.h
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#endif

#include <cpr/cpr.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace atomicripper::verify {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Read a little-endian uint32 from raw bytes at position pos.
uint32_t readLE32(const uint8_t* buf, size_t pos) {
    return static_cast<uint32_t>(buf[pos])
         | (static_cast<uint32_t>(buf[pos + 1]) << 8)
         | (static_cast<uint32_t>(buf[pos + 2]) << 16)
         | (static_cast<uint32_t>(buf[pos + 3]) << 24);
}

// Rotate uint32 right by N bits.
inline uint32_t ror32(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

// Frame offset used in AccurateRip ID math = LBA + 150 (physical frame address)
inline uint32_t arOffset(uint32_t lba) { return lba + 150; }

// Sum of decimal digits of n
uint32_t digitSum(uint32_t n) {
    uint32_t s = 0;
    while (n > 0) { s += n % 10; n /= 10; }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Checksum V1
//   crc += frame_value * (1-based frame index)
//   Skip: first 2939 frames of first track, last 2940 frames of last track
// ---------------------------------------------------------------------------
uint32_t AccurateRip::checksumV1(const uint8_t* pcm, size_t bytes,
                                  bool isFirst, bool isLast) {
    const size_t frameCount  = bytes / 4;   // 4 bytes per stereo frame
    const size_t skipStart   = isFirst ? 2939u : 0u;
    const size_t skipEnd     = isLast  ? 2940u : 0u;

    uint32_t crc = 0;
    for (size_t i = 0; i < frameCount; ++i) {
        if (i < skipStart)                      continue;
        if (isLast && i >= frameCount - skipEnd) continue;

        // Raw 32-bit stereo frame: L[15:0] | R[31:16]  (little-endian in memory)
        const uint32_t frame =
            static_cast<uint32_t>(pcm[i * 4    ])        |
            (static_cast<uint32_t>(pcm[i * 4 + 1]) <<  8) |
            (static_cast<uint32_t>(pcm[i * 4 + 2]) << 16) |
            (static_cast<uint32_t>(pcm[i * 4 + 3]) << 24);

        crc += frame * static_cast<uint32_t>(i + 1);
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Checksum V2
//   product = frame_value * (1-based frame index)
//   crc    += ROR32(product, 8)
//   (same skip logic as V1)
// ---------------------------------------------------------------------------
uint32_t AccurateRip::checksumV2(const uint8_t* pcm, size_t bytes,
                                  bool isFirst, bool isLast) {
    const size_t frameCount = bytes / 4;
    const size_t skipStart  = isFirst ? 2939u : 0u;
    const size_t skipEnd    = isLast  ? 2940u : 0u;

    uint32_t crc = 0;
    for (size_t i = 0; i < frameCount; ++i) {
        if (i < skipStart)                       continue;
        if (isLast && i >= frameCount - skipEnd) continue;

        const uint32_t frame =
            static_cast<uint32_t>(pcm[i * 4    ])        |
            (static_cast<uint32_t>(pcm[i * 4 + 1]) <<  8) |
            (static_cast<uint32_t>(pcm[i * 4 + 2]) << 16) |
            (static_cast<uint32_t>(pcm[i * 4 + 3]) << 24);

        crc += ror32(frame * static_cast<uint32_t>(i + 1), 8);
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Disc ID 1 — sum of all physical frame offsets (tracks + lead-out)
// ---------------------------------------------------------------------------
uint32_t AccurateRip::discId1(const drive::TOC& toc) {
    uint32_t id = 0;
    for (const auto& t : toc.tracks)
        id += arOffset(t.lba);
    id += arOffset(toc.leadOutLBA);
    return id;
}

// ---------------------------------------------------------------------------
// Disc ID 2 — weighted sum of frame offsets
// ---------------------------------------------------------------------------
uint32_t AccurateRip::discId2(const drive::TOC& toc) {
    uint32_t id = 0;
    for (const auto& t : toc.tracks)
        id += std::max(1, t.number) * std::max(1u, arOffset(t.lba));
    id += static_cast<uint32_t>(toc.tracks.size() + 1) * arOffset(toc.leadOutLBA);
    return id;
}

// ---------------------------------------------------------------------------
// CDDB ID — classic CDDB checksum using physical frame offsets
// ---------------------------------------------------------------------------
uint32_t AccurateRip::cddbId(const drive::TOC& toc) {
    uint32_t digitSumTotal = 0;
    for (const auto& t : toc.tracks)
        digitSumTotal += digitSum(arOffset(t.lba) / 75);

    const uint32_t discLengthSecs =
        arOffset(toc.leadOutLBA) / 75 - arOffset(toc.tracks.front().lba) / 75;
    const uint32_t numTracks = static_cast<uint32_t>(toc.tracks.size());

    return ((digitSumTotal % 255) << 24) | (discLengthSecs << 8) | numTracks;
}

// ---------------------------------------------------------------------------
// Build AccurateRip URL
//   http://www.accuraterip.com/accuraterip/{a}/{b}/{c}/dBAR-{nnn}-{id1}-{id2}-{cddb}.bin
//   where a = id1 & 0xF, b = (id1>>4)&0xF, c = (id1>>8)&0xF  (path subdirs)
// ---------------------------------------------------------------------------
std::string AccurateRip::buildUrl(const drive::TOC& toc) {
    const uint32_t id1   = discId1(toc);
    const uint32_t id2   = discId2(toc);
    const uint32_t cddb  = cddbId(toc);
    const int      ntracks = static_cast<int>(toc.tracks.size());

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "http://www.accuraterip.com/accuraterip/%x/%x/%x/"
        "dBAR-%03d-%08x-%08x-%08x.bin",
        id1 & 0xFu, (id1 >> 4) & 0xFu, (id1 >> 8) & 0xFu,
        ntracks, id1, id2, cddb);
    return buf;
}

// ---------------------------------------------------------------------------
// Binary response parser
//
// Format (repeating chunks):
//   [0]     uint8   track count in this entry
//   [1..4]  uint32  disc id1   (LE)
//   [5..8]  uint32  disc id2   (LE)
//   [9..12] uint32  CDDB id    (LE)
//   Per track (9 bytes):
//     [0]     uint8   confidence
//     [1..4]  uint32  CRC v1   (LE)
//     [5..8]  uint32  CRC v2   (LE)
// ---------------------------------------------------------------------------
namespace {

struct ArDbTrack {
    uint8_t  confidence;
    uint32_t crcV1;
    uint32_t crcV2;
};

struct ArDbEntry {
    uint8_t  trackCount;
    uint32_t id1, id2, cddb;
    std::vector<ArDbTrack> tracks;
};

std::vector<ArDbEntry> parseArBinary(const std::string& data) {
    std::vector<ArDbEntry> entries;
    const auto* buf = reinterpret_cast<const uint8_t*>(data.data());
    const size_t len = data.size();

    size_t pos = 0;
    while (pos + 13 <= len) {
        ArDbEntry entry;
        entry.trackCount = buf[pos++];

        if (entry.trackCount == 0) break;  // malformed

        entry.id1  = readLE32(buf, pos); pos += 4;
        entry.id2  = readLE32(buf, pos); pos += 4;
        entry.cddb = readLE32(buf, pos); pos += 4;

        const size_t trackBytes = static_cast<size_t>(entry.trackCount) * 9u;
        if (pos + trackBytes > len) break;  // truncated

        for (int t = 0; t < entry.trackCount; ++t) {
            ArDbTrack tr;
            tr.confidence = buf[pos++];
            tr.crcV1 = readLE32(buf, pos); pos += 4;
            tr.crcV2 = readLE32(buf, pos); pos += 4;
            entry.tracks.push_back(tr);
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace

// ---------------------------------------------------------------------------
// verify() — HTTP fetch + match
// ---------------------------------------------------------------------------
ArDiscResult AccurateRip::verify(const drive::TOC& toc,
                                  const std::vector<std::vector<uint8_t>>& trackPcm) {
    ArDiscResult result;
    result.url = buildUrl(toc);

    // Only consider audio tracks
    std::vector<const drive::TrackInfo*> audioTracks;
    for (const auto& t : toc.tracks)
        if (t.isAudio) audioTracks.push_back(&t);

    if (audioTracks.empty() || trackPcm.size() < audioTracks.size()) {
        result.error = "no audio tracks or mismatched track data";
        return result;
    }

    // -- Compute our checksums (offset = 0) ---------------------------------
    result.tracks.resize(audioTracks.size());
    for (size_t i = 0; i < audioTracks.size(); ++i) {
        const bool isFirst = (i == 0);
        const bool isLast  = (i == audioTracks.size() - 1);
        const auto& pcm    = trackPcm[i];

        result.tracks[i].trackNumber = audioTracks[i]->number;
        result.tracks[i].checksumV1  =
            checksumV1(pcm.data(), pcm.size(), isFirst, isLast);
        result.tracks[i].checksumV2  =
            checksumV2(pcm.data(), pcm.size(), isFirst, isLast);
    }

    // -- HTTP fetch ---------------------------------------------------------
    auto response = cpr::Get(
        cpr::Url{result.url},
        cpr::Timeout{15000},  // 15 second timeout
        cpr::Header{{"User-Agent", "AtomicRipper/0.4.0"}}
    );

    if (response.status_code == 404) {
        // Disc not in AccurateRip database yet — not an error per se
        result.lookupOk = true;
        result.error    = "disc not found in AccurateRip database";
        return result;
    }
    if (response.status_code != 200 || response.text.empty()) {
        result.error = "HTTP error " + std::to_string(response.status_code);
        return result;
    }

    // -- Parse binary response ----------------------------------------------
    auto dbEntries = parseArBinary(response.text);
    result.dbEntries = static_cast<int>(dbEntries.size());
    result.lookupOk  = true;

    if (dbEntries.empty()) {
        result.error = "could not parse AccurateRip response";
        return result;
    }

    // -- Match each of our tracks against every DB entry -------------------
    // Accumulate the highest confidence seen for each track.
    for (const auto& entry : dbEntries) {
        if (entry.trackCount != static_cast<uint8_t>(audioTracks.size()))
            continue;  // different pressing with different track count — skip

        for (size_t i = 0; i < result.tracks.size() && i < entry.tracks.size(); ++i) {
            auto& ours = result.tracks[i];
            const auto& db = entry.tracks[i];

            if (db.crcV1 == ours.checksumV1)
                ours.confidenceV1 = std::max(ours.confidenceV1,
                                             static_cast<int>(db.confidence));
            if (db.crcV2 == ours.checksumV2)
                ours.confidenceV2 = std::max(ours.confidenceV2,
                                             static_cast<int>(db.confidence));
            if (ours.confidenceV1 > 0 || ours.confidenceV2 > 0)
                ours.matched = true;
        }
    }

    return result;
}

} // namespace atomicripper::verify
