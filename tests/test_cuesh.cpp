#include <catch2/catch_test_macros.hpp>
#include <core/drive/TOC.hpp>
#include <core/metadata/CueSheet.hpp>
#include <core/metadata/MusicBrainz.hpp>

#include <string>
#include <vector>

using namespace atomicripper;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static drive::TOC make3TrackToc() {
    drive::TOC toc;
    toc.firstTrack = 1;
    toc.lastTrack  = 3;
    toc.leadOutLBA = 50000;
    for (int i = 0; i < 3; ++i) {
        drive::TrackInfo t;
        t.number      = i + 1;
        t.isAudio     = true;
        t.lba         = static_cast<uint32_t>(i * 15000);
        t.sectorCount = 15000;
        toc.tracks.push_back(t);
    }
    return toc;
}

static metadata::MbRelease makeRelease() {
    metadata::MbRelease rel;
    rel.artist  = "Test Artist";
    rel.title   = "Test Album";
    rel.date    = "2024-01-01";
    rel.label   = "Test Label";
    rel.country = "US";
    metadata::MbTrack t1; t1.title = "Track One";   t1.artist = "";
    metadata::MbTrack t2; t2.title = "Track Two";   t2.artist = "Guest";
    metadata::MbTrack t3; t3.title = "Track Three"; t3.artist = "";
    rel.tracks = { t1, t2, t3 };
    return rel;
}

// ---------------------------------------------------------------------------
// filename()
// ---------------------------------------------------------------------------

TEST_CASE("CueSheet::filename — nullptr release → disc.cue", "[cuesheet]") {
    REQUIRE(metadata::CueSheet::filename(nullptr) == "disc.cue");
}

TEST_CASE("CueSheet::filename — derives from artist + title", "[cuesheet]") {
    auto rel = makeRelease();
    std::string name = metadata::CueSheet::filename(&rel);
    REQUIRE(name.find("Test Artist") != std::string::npos);
    REQUIRE(name.find("Test Album")  != std::string::npos);
    REQUIRE(name.substr(name.size() - 4) == ".cue");
}

TEST_CASE("CueSheet::filename — sanitises illegal characters", "[cuesheet]") {
    metadata::MbRelease rel;
    rel.artist = "AC/DC";
    rel.title  = "Back: In Black";
    std::string name = metadata::CueSheet::filename(&rel);
    REQUIRE(name.find('/') == std::string::npos);
    REQUIRE(name.find(':') == std::string::npos);
}

// ---------------------------------------------------------------------------
// generate() — per-track mode
// ---------------------------------------------------------------------------

TEST_CASE("CueSheet::generate — per-track: one FILE per track", "[cuesheet]") {
    auto toc = make3TrackToc();
    std::vector<std::filesystem::path> files = {
        "01 - Track One.flac", "02 - Track Two.flac", "03 - Track Three.flac"
    };
    std::string cue = metadata::CueSheet::generate(toc, files, nullptr, "DISCID123");

    // Should have exactly 3 FILE lines
    size_t count = 0;
    size_t pos = 0;
    while ((pos = cue.find("FILE \"", pos)) != std::string::npos) { ++count; ++pos; }
    REQUIRE(count == 3);
}

TEST_CASE("CueSheet::generate — per-track: each INDEX 01 is 00:00:00", "[cuesheet]") {
    auto toc = make3TrackToc();
    std::vector<std::filesystem::path> files = {
        "01.flac", "02.flac", "03.flac"
    };
    std::string cue = metadata::CueSheet::generate(toc, files, nullptr);

    // All INDEX 01 lines should be at 00:00:00
    size_t pos = 0;
    int found = 0;
    while ((pos = cue.find("INDEX 01 00:00:00", pos)) != std::string::npos) {
        ++found; ++pos;
    }
    REQUIRE(found == 3);
}

TEST_CASE("CueSheet::generate — per-track: includes track metadata", "[cuesheet]") {
    auto toc = make3TrackToc();
    auto rel  = makeRelease();
    std::vector<std::filesystem::path> files = { "01.flac", "02.flac", "03.flac" };
    std::string cue = metadata::CueSheet::generate(toc, files, &rel, "DISCID");

    REQUIRE(cue.find("TITLE \"Test Album\"")  != std::string::npos);
    REQUIRE(cue.find("PERFORMER \"Test Artist\"") != std::string::npos);
    REQUIRE(cue.find("TITLE \"Track One\"")   != std::string::npos);
    REQUIRE(cue.find("TITLE \"Track Two\"")   != std::string::npos);
    REQUIRE(cue.find("PERFORMER \"Guest\"")   != std::string::npos);  // track-level artist
    REQUIRE(cue.find("REM DATE 2024")         != std::string::npos);
    REQUIRE(cue.find("REM LABEL \"Test Label\"") != std::string::npos);
}

TEST_CASE("CueSheet::generate — uses CRLF line endings", "[cuesheet]") {
    auto toc = make3TrackToc();
    std::string cue = metadata::CueSheet::generate(toc, {}, nullptr);
    REQUIRE(cue.find("\r\n") != std::string::npos);
    // No bare LF should appear (every LF must be preceded by CR)
    size_t pos = 0;
    while ((pos = cue.find('\n', pos)) != std::string::npos) {
        REQUIRE(pos > 0);
        REQUIRE(cue[pos - 1] == '\r');
        ++pos;
    }
}

// ---------------------------------------------------------------------------
// generate() — single-file mode
// ---------------------------------------------------------------------------

TEST_CASE("CueSheet::generate — single-file: exactly one FILE line", "[cuesheet]") {
    auto toc = make3TrackToc();
    std::filesystem::path sf = "disc.flac";
    // 3 tracks, each starting at 0 / 15000*588 / 30000*588 interleaved samples
    // But for the test we just use round sector-aligned offsets (multiples of 588)
    const uint64_t s = 588u;  // 1 sector = 588 stereo frames
    std::vector<uint64_t> offsets = { 0, 15000 * s, 30000 * s };

    std::string cue = metadata::CueSheet::generate(toc, { sf }, nullptr, {}, offsets);

    size_t count = 0;
    size_t pos = 0;
    while ((pos = cue.find("FILE \"", pos)) != std::string::npos) { ++count; ++pos; }
    REQUIRE(count == 1);
    REQUIRE(cue.find("disc.flac") != std::string::npos);
}

TEST_CASE("CueSheet::generate — single-file: track 1 INDEX at 00:00:00", "[cuesheet]") {
    auto toc = make3TrackToc();
    std::vector<uint64_t> offsets = { 0, 15000 * 588u, 30000 * 588u };
    std::string cue = metadata::CueSheet::generate(toc, { "disc.flac" }, nullptr, {}, offsets);

    // Track 1 must start at 00:00:00 (offset 0 → LBA 0 → MM:SS:FF = 00:00:00)
    REQUIRE(cue.find("INDEX 01 00:00:00") != std::string::npos);
}

TEST_CASE("CueSheet::generate — single-file: track 2 INDEX offset > 00:00:00", "[cuesheet]") {
    auto toc = make3TrackToc();
    // Track 2 at sector 15000 → MSF = 15000/75 sec = 200s = 3m20s → 03:20:00
    std::vector<uint64_t> offsets = { 0, 15000 * 588u, 30000 * 588u };
    std::string cue = metadata::CueSheet::generate(toc, { "disc.flac" }, nullptr, {}, offsets);

    REQUIRE(cue.find("INDEX 01 03:20:00") != std::string::npos);
}
