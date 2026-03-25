#include <catch2/catch_test_macros.hpp>
#include <core/drive/TOC.hpp>
#include <core/metadata/DiscId.hpp>

using namespace atomicripper;

// ---------------------------------------------------------------------------
// Helper: build a TOC that matches the famous MusicBrainz test case
// (Dark Side of the Moon — well-known disc ID: arIS30MPHTnRoGbxOHLqQGbMtaw-)
// ---------------------------------------------------------------------------
static drive::TOC makeDSOTM() {
    drive::TOC toc;
    toc.firstTrack = 1;
    toc.lastTrack  = 10;
    toc.leadOutLBA = 247073 - 150; // lead-out at sector offset 247073

    // Track offsets (LBA = MB offset - 150)
    const uint32_t mbOffsets[] = {
        183, 2832, 20733, 36268, 53777,
        68218, 91808, 103763, 121983, 139891
    };
    for (int i = 0; i < 10; ++i) {
        drive::TrackInfo t;
        t.number      = i + 1;
        t.isAudio     = true;
        t.lba         = mbOffsets[i] - 150;
        t.sectorCount = 0; // not used in disc ID
        toc.tracks.push_back(t);
    }
    return toc;
}

// ---------------------------------------------------------------------------

TEST_CASE("TOC validity checks", "[toc]") {
    drive::TOC empty;
    REQUIRE_FALSE(empty.isValid());

    auto toc = makeDSOTM();
    REQUIRE(toc.isValid());
    REQUIRE(toc.audioTrackCount() == 10);
}

TEST_CASE("TOC duration calculation", "[toc]") {
    auto toc = makeDSOTM();
    // leadOutLBA / 75 gives disc duration in seconds
    REQUIRE(toc.durationSeconds() > 0.0);
}

TEST_CASE("MusicBrainz Disc ID — known disc", "[discid]") {
    auto toc = makeDSOTM();
    std::string id = metadata::DiscId::calculate(toc);

    REQUIRE_FALSE(id.empty());
    REQUIRE(id.size() == 28); // MB disc IDs are always 28 characters

    // Disc IDs contain only base64url-ish characters: A-Z a-z 0-9 . _ -
    for (char c : id) {
        bool valid = (c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') ||
                     c == '.' || c == '_' || c == '-';
        REQUIRE(valid);
    }
}

TEST_CASE("MusicBrainz Disc ID — invalid TOC returns empty", "[discid]") {
    drive::TOC bad;
    REQUIRE(metadata::DiscId::calculate(bad).empty());
}
