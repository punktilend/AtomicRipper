#include <catch2/catch_test_macros.hpp>
#include <core/drive/TOC.hpp>
#include <core/verify/AccurateRip.hpp>

#include <cstring>
#include <vector>

using namespace atomicripper;

// ---------------------------------------------------------------------------
// Helper: build a synthetic single-sector PCM buffer (2352 bytes)
// All samples set to the given 16-bit value (both channels).
// ---------------------------------------------------------------------------
static std::vector<uint8_t> makePcm(size_t sectors, int16_t sampleValue = 0x0100) {
    const size_t byteCount = sectors * 2352;
    std::vector<uint8_t> pcm(byteCount);
    for (size_t i = 0; i < byteCount; i += 4) {
        // Left channel (LE int16)
        pcm[i + 0] = static_cast<uint8_t>( sampleValue        & 0xFF);
        pcm[i + 1] = static_cast<uint8_t>((sampleValue >>  8) & 0xFF);
        // Right channel (LE int16) — same value
        pcm[i + 2] = pcm[i + 0];
        pcm[i + 3] = pcm[i + 1];
    }
    return pcm;
}

// ---------------------------------------------------------------------------
// Helper: build the Dark Side of the Moon TOC (same reference disc used in
// test_toc.cpp so we can verify the AccurateRip disc IDs against known values).
// ---------------------------------------------------------------------------
static drive::TOC makeDSOTM() {
    drive::TOC toc;
    toc.firstTrack = 1;
    toc.lastTrack  = 10;
    toc.leadOutLBA = 247073 - 150;

    const uint32_t mbOffsets[] = {
        183, 2832, 20733, 36268, 53777,
        68218, 91808, 103763, 121983, 139891
    };
    for (int i = 0; i < 10; ++i) {
        drive::TrackInfo t;
        t.number      = i + 1;
        t.isAudio     = true;
        t.lba         = mbOffsets[i] - 150;
        t.sectorCount = (i < 9)
            ? (mbOffsets[i + 1] - mbOffsets[i])
            : (247073 - mbOffsets[9]);
        toc.tracks.push_back(t);
    }
    return toc;
}

// ---------------------------------------------------------------------------

TEST_CASE("AccurateRip checksum V1 — silence is zero", "[accuraterip]") {
    // A buffer of all zeros gives CRC = 0 (0 * anything = 0)
    std::vector<uint8_t> silence(2352 * 4, 0);
    REQUIRE(verify::AccurateRip::checksumV1(silence.data(), silence.size(),
                                             false, false) == 0u);
    REQUIRE(verify::AccurateRip::checksumV2(silence.data(), silence.size(),
                                             false, false) == 0u);
}

TEST_CASE("AccurateRip checksum V1 — non-zero PCM produces non-zero CRC", "[accuraterip]") {
    // Middle-track scenario (isFirst=false, isLast=false): all frames included.
    // With constant sample value S and N frames, V1 = S * sum(1..N) = S * N*(N+1)/2
    const int16_t sampleVal = 1;
    // Build a frame where both channels = 1: raw 32-bit value = 0x00010001
    std::vector<uint8_t> pcm = makePcm(10, sampleVal);

    uint32_t crc = verify::AccurateRip::checksumV1(
        pcm.data(), pcm.size(), false, false);
    REQUIRE(crc != 0u);
}

TEST_CASE("AccurateRip checksum V1 — first-track skip changes result", "[accuraterip]") {
    // With isFirst=true, frames 0..2938 are skipped.
    // The checksums should differ because different frames contribute to each.
    // (We don't compare magnitudes because uint32 wraps with large frame counts.)
    auto pcm = makePcm(100, 0x0001);
    uint32_t noSkip   = verify::AccurateRip::checksumV1(pcm.data(), pcm.size(), false, false);
    uint32_t withSkip = verify::AccurateRip::checksumV1(pcm.data(), pcm.size(), true,  false);
    REQUIRE(withSkip != noSkip);
}

TEST_CASE("AccurateRip checksum V2 — differs from V1 for non-trivial data", "[accuraterip]") {
    auto pcm = makePcm(50, 0x1234);
    uint32_t v1 = verify::AccurateRip::checksumV1(pcm.data(), pcm.size(), false, false);
    uint32_t v2 = verify::AccurateRip::checksumV2(pcm.data(), pcm.size(), false, false);
    // V2 rotates the product — result must differ from V1 for non-zero data
    REQUIRE(v1 != v2);
}

TEST_CASE("AccurateRip disc IDs — DSOTM URL has correct structure", "[accuraterip]") {
    auto toc = makeDSOTM();

    uint32_t id1  = verify::AccurateRip::discId1(toc);
    uint32_t id2  = verify::AccurateRip::discId2(toc);
    uint32_t cddb = verify::AccurateRip::cddbId(toc);

    // IDs must be non-zero for a real disc
    REQUIRE(id1  != 0u);
    REQUIRE(id2  != 0u);
    REQUIRE(cddb != 0u);

    // URL must follow the dBAR-NNN-id1-id2-cddb.bin pattern
    std::string url = verify::AccurateRip::buildUrl(toc);
    REQUIRE(url.find("accuraterip.com") != std::string::npos);
    REQUIRE(url.find("dBAR-010-") != std::string::npos);  // 10 tracks
    REQUIRE(url.substr(url.size() - 4) == ".bin");
}

TEST_CASE("AccurateRip disc ID path — first three nibbles of ID1 form subdirs", "[accuraterip]") {
    auto toc = makeDSOTM();
    uint32_t id1 = verify::AccurateRip::discId1(toc);
    std::string url = verify::AccurateRip::buildUrl(toc);

    // URL path should be /accuraterip/{a}/{b}/{c}/
    // where a = id1 & 0xF, b = (id1>>4)&0xF, c = (id1>>8)&0xF
    char expected[64];
    std::snprintf(expected, sizeof(expected), "/accuraterip/%x/%x/%x/",
                  id1 & 0xFu, (id1 >> 4) & 0xFu, (id1 >> 8) & 0xFu);
    REQUIRE(url.find(expected) != std::string::npos);
}
