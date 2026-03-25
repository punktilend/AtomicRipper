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

// ---------------------------------------------------------------------------
// detectOffset() — find drive read offset using incremental V1 CRC sweep
//
// Key identity (allows O(1) update per offset step):
//   crc1(O) = Σ frame[j]·(j+1)  −  (Fk + O) · Σ frame[j]
//           = a(O)              −  (Fk + O) · b(O)
// where the sums slide over j ∈ [Fk + skipStart + O, Fk + N − skipEnd − 1 + O]
// and Fk = frame index where track k starts in the concatenated frame array.
//
// Sliding update when O → O+1  (window moves right by one frame):
//   a(O+1) = a(O) − frames[lo]·(lo+1) + frames[hi+1]·(hi+2)
//   b(O+1) = b(O) − frames[lo]        + frames[hi+1]
// ---------------------------------------------------------------------------
ArOffsetResult AccurateRip::detectOffset(
    const drive::TOC&                        toc,
    const std::vector<std::vector<uint8_t>>& allTrackPcm,
    int                                      maxOffset)
{
    ArOffsetResult result;

    // Collect audio tracks
    std::vector<const drive::TrackInfo*> audioTracks;
    for (const auto& t : toc.tracks)
        if (t.isAudio) audioTracks.push_back(&t);

    const int T = static_cast<int>(audioTracks.size());
    if (T == 0 || static_cast<int>(allTrackPcm.size()) < T) {
        result.error = "no audio track data";
        return result;
    }

    // ------------------------------------------------------------------
    // 1.  Build concatenated uint32 frame array + per-track metadata
    // ------------------------------------------------------------------
    std::vector<uint32_t> frames;
    frames.reserve([&] {
        size_t total = 0;
        for (int i = 0; i < T; ++i) total += allTrackPcm[i].size() / 4;
        return total;
    }());

    std::vector<size_t> trackFrameStart(T);
    std::vector<size_t> trackFrameCount(T);

    for (int k = 0; k < T; ++k) {
        trackFrameStart[k] = frames.size();
        const auto& pcm    = allTrackPcm[k];
        const size_t fc    = pcm.size() / 4;
        trackFrameCount[k] = fc;
        for (size_t j = 0; j < fc; ++j) {
            frames.push_back(
                static_cast<uint32_t>(pcm[j*4    ])        |
                (static_cast<uint32_t>(pcm[j*4+1]) <<  8)  |
                (static_cast<uint32_t>(pcm[j*4+2]) << 16)  |
                (static_cast<uint32_t>(pcm[j*4+3]) << 24));
        }
    }

    // ------------------------------------------------------------------
    // 2.  Fetch & parse AccurateRip DB
    // ------------------------------------------------------------------
    const std::string url = buildUrl(toc);
    auto resp = cpr::Get(
        cpr::Url{url},
        cpr::Timeout{15000},
        cpr::Header{{"User-Agent", "AtomicRipper/0.6"}});

    if (resp.status_code == 404) {
        result.error = "disc not found in AccurateRip database";
        return result;
    }
    if (resp.status_code != 200 || resp.text.empty()) {
        result.error = "HTTP error " + std::to_string(resp.status_code);
        return result;
    }

    auto dbEntries = parseArBinary(resp.text);
    if (dbEntries.empty()) {
        result.error = "could not parse AccurateRip response";
        return result;
    }

    // ------------------------------------------------------------------
    // 3.  Build expected-CRC table: per track, all V1 CRCs from all pressings
    // ------------------------------------------------------------------
    struct Expected { uint32_t crcV1; int confidence; };
    std::vector<std::vector<Expected>> expected(T);

    for (const auto& entry : dbEntries) {
        if (entry.trackCount != static_cast<uint8_t>(T)) continue;
        for (int i = 0; i < T && i < static_cast<int>(entry.tracks.size()); ++i) {
            const auto& et = entry.tracks[static_cast<size_t>(i)];
            if (et.crcV1 != 0)
                expected[i].push_back({et.crcV1, static_cast<int>(et.confidence)});
        }
    }

    {
        bool anyExpected = false;
        for (int i = 0; i < T; ++i)
            if (!expected[i].empty()) { anyExpected = true; break; }
        if (!anyExpected) {
            result.error = "no matching track count in AccurateRip entries";
            return result;
        }
    }

    // ------------------------------------------------------------------
    // 4.  Initialise sliding window state for O = −maxOffset
    // ------------------------------------------------------------------
    constexpr size_t SKIP_FIRST = 2939u;
    constexpr size_t SKIP_LAST  = 2940u;

    struct TrackState {
        uint32_t a     = 0;   // Σ frames[j]·(j+1) over [lo, hi]
        uint32_t b     = 0;   // Σ frames[j] over [lo, hi]
        size_t   lo    = 0;   // current window start (inclusive)
        size_t   hi    = 0;   // current window end   (inclusive)
        size_t   Fk    = 0;   // frame start of track k in `frames`
        bool     valid = false;
    };

    std::vector<TrackState> st(T);

    for (int k = 0; k < T; ++k) {
        const bool   isFirst = (k == 0);
        const bool   isLast  = (k == T - 1);
        const size_t Fk      = trackFrameStart[k];
        const size_t Nk      = trackFrameCount[k];
        const size_t sk      = isFirst ? SKIP_FIRST : 0u;
        const size_t ek      = isLast  ? SKIP_LAST  : 0u;

        // Window at O = -maxOffset
        const ptrdiff_t loI = static_cast<ptrdiff_t>(Fk + sk) - maxOffset;
        const ptrdiff_t hiI = static_cast<ptrdiff_t>(Fk + Nk - ek) - 1 - maxOffset;

        if (loI < 0 || hiI < loI ||
            static_cast<size_t>(hiI) >= frames.size())
            continue;  // this track's window is out of range — skip it

        st[k].lo    = static_cast<size_t>(loI);
        st[k].hi    = static_cast<size_t>(hiI);
        st[k].Fk    = Fk;
        st[k].valid = true;

        // O(N) initial sum — done once per track
        for (size_t j = st[k].lo; j <= st[k].hi; ++j) {
            st[k].a += frames[j] * static_cast<uint32_t>(j + 1);
            st[k].b += frames[j];
        }
    }

    // ------------------------------------------------------------------
    // 5.  Sweep O from −maxOffset to +maxOffset; O(1) update per step
    // ------------------------------------------------------------------
    int bestScore    = -1;
    int bestOffset   = 0;
    int bestMatched  = 0;

    for (int O = -maxOffset; O <= maxOffset; ++O) {
        int totalConf = 0, matched = 0;

        for (int k = 0; k < T; ++k) {
            if (!st[k].valid) continue;

            // crc1(O) = a - (Fk + O) * b  (all uint32, mod 2^32)
            const uint32_t Fk_plus_O =
                static_cast<uint32_t>(
                    static_cast<ptrdiff_t>(st[k].Fk) + O);
            const uint32_t crc1 = st[k].a - Fk_plus_O * st[k].b;

            for (const auto& e : expected[k]) {
                if (e.crcV1 == crc1) {
                    totalConf += e.confidence;
                    ++matched;
                    break;
                }
            }
        }

        if (totalConf > bestScore ||
            (totalConf == bestScore && matched > bestMatched)) {
            bestScore   = totalConf;
            bestOffset  = O;
            bestMatched = matched;
        }

        // Advance window (skip on final iteration)
        if (O < maxOffset) {
            for (int k = 0; k < T; ++k) {
                if (!st[k].valid) continue;

                const size_t oldLo = st[k].lo;
                const size_t newHi = st[k].hi + 1;
                if (newHi >= frames.size()) { st[k].valid = false; continue; }

                st[k].a -= frames[oldLo] * static_cast<uint32_t>(oldLo + 1);
                st[k].b -= frames[oldLo];
                st[k].a += frames[newHi] * static_cast<uint32_t>(newHi + 1);
                st[k].b += frames[newHi];
                st[k].lo++;
                st[k].hi++;
            }
        }
    }

    result.sampleOffset  = bestOffset;
    result.confidence    = bestScore > 0 ? bestScore : 0;
    result.tracksMatched = bestMatched;
    result.found         = (bestMatched > 0);
    if (!result.found)
        result.error = "no offset produced a match against the AccurateRip database";

    return result;
}

} // namespace atomicripper::verify
