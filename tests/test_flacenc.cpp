#include <catch2/catch_test_macros.hpp>
#include <core/encode/FlacEncoder.hpp>
#include <core/encode/IEncoder.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace atomicripper::encode;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// RAII temp file — deleted in destructor even if the test throws.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const char* suffix = ".flac") {
        path = std::filesystem::temp_directory_path()
             / ("ar_test_XXXXXXXX" + std::string(suffix));
        // Make the filename unique without requiring platform tmpnam tricks
        path.replace_filename(
            "ar_test_" + std::to_string(
                std::hash<std::string>{}(path.string())) + suffix);
    }
    ~TempFile() { std::error_code ec; std::filesystem::remove(path, ec); }
};

// Produce N stereo frames of silence (int32_t interleaved, 16-bit range).
static std::vector<int32_t> silence(size_t frames) {
    return std::vector<int32_t>(frames * 2, 0);
}

// Produce N stereo frames with a fixed sample value.
static std::vector<int32_t> tone(size_t frames, int32_t value) {
    return std::vector<int32_t>(frames * 2, value);
}

// ---------------------------------------------------------------------------
// Basic encode round-trip
// ---------------------------------------------------------------------------

TEST_CASE("FlacEncoder — encode silence produces a valid file", "[flacenc]") {
    TempFile tmp;
    FlacEncoder enc;

    EncoderSettings es;
    es.totalSamples  = 44100;           // 1 second
    es.compressionLevel = 0;            // fastest — keeps tests snappy

    REQUIRE(enc.open(tmp.path, es));

    auto pcm = silence(44100);
    REQUIRE(enc.writeSamples(pcm));
    REQUIRE(enc.finalize());

    // File must exist and be non-empty (at minimum the FLAC stream header)
    REQUIRE(std::filesystem::exists(tmp.path));
    REQUIRE(std::filesystem::file_size(tmp.path) > 0);
}

TEST_CASE("FlacEncoder — encode with tags succeeds and file is non-empty", "[flacenc]") {
    TempFile taggedFile;
    EncoderSettings es;
    es.totalSamples     = 4410;
    es.compressionLevel = 0;

    FlacEncoder enc;
    enc.setTag("TITLE",       "Test Song");
    enc.setTag("ARTIST",      "Test Artist");
    enc.setTag("ALBUM",       "Test Album");
    enc.setTag("TRACKNUMBER", "1");
    REQUIRE(enc.open(taggedFile.path, es));
    REQUIRE(enc.writeSamples(silence(4410)));
    REQUIRE(enc.finalize());

    // File must exist and be non-trivially sized (audio + metadata)
    REQUIRE(std::filesystem::exists(taggedFile.path));
    REQUIRE(std::filesystem::file_size(taggedFile.path) > 100);
}

TEST_CASE("FlacEncoder — clearTags removes previously set tags", "[flacenc]") {
    // Clearing tags and then encoding should produce the same-sized file as
    // encoding with no tags (within the padding block tolerance).
    TempFile f1, f2;
    EncoderSettings es;
    es.totalSamples     = 4410;
    es.compressionLevel = 0;

    auto encode = [&](const std::filesystem::path& p, bool withTags) {
        FlacEncoder enc;
        if (withTags) {
            enc.setTag("TITLE", "Will be cleared");
            enc.clearTags();
        }
        REQUIRE(enc.open(p, es));
        REQUIRE(enc.writeSamples(silence(4410)));
        REQUIRE(enc.finalize());
    };

    encode(f1.path, false);
    encode(f2.path, true);   // set tags then clear — should equal no-tag file

    REQUIRE(std::filesystem::file_size(f1.path) ==
            std::filesystem::file_size(f2.path));
}

TEST_CASE("FlacEncoder — non-zero PCM compresses to a file", "[flacenc]") {
    // Non-silence should still produce a valid FLAC (may or may not compress
    // well, but must not error).
    TempFile tmp;
    FlacEncoder enc;
    EncoderSettings es;
    es.totalSamples     = 8820;
    es.compressionLevel = 0;

    REQUIRE(enc.open(tmp.path, es));

    // Alternate +/- values to produce a basic square wave
    std::vector<int32_t> pcm(8820 * 2);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = (i % 4 < 2) ? 10000 : -10000;

    REQUIRE(enc.writeSamples(pcm));
    REQUIRE(enc.finalize());
    REQUIRE(std::filesystem::file_size(tmp.path) > 0);
}

TEST_CASE("FlacEncoder — writeSamples after finalize returns false", "[flacenc]") {
    TempFile tmp;
    FlacEncoder enc;
    EncoderSettings es;
    es.totalSamples     = 4410;
    es.compressionLevel = 0;

    REQUIRE(enc.open(tmp.path, es));
    REQUIRE(enc.writeSamples(silence(4410)));
    REQUIRE(enc.finalize());

    // Any subsequent write must fail gracefully
    auto extra = silence(100);
    REQUIRE_FALSE(enc.writeSamples(extra));
    REQUIRE_FALSE(enc.lastError().empty());
}

// ---------------------------------------------------------------------------
// CUESHEET block
// ---------------------------------------------------------------------------

TEST_CASE("FlacEncoder — embedded cuesheet: file still produces valid output", "[flacenc]") {
    TempFile tmp;
    FlacEncoder enc;

    // Simulate a 2-track disc, 2 s per track
    const uint64_t framesPerTrack = 44100 * 2;
    const uint64_t totalFrames    = framesPerTrack * 2;

    enc.addCueSheetTrack({ 1, 0,             true });
    enc.addCueSheetTrack({ 2, framesPerTrack, true });
    enc.setCueSheetLeadOut(totalFrames);

    EncoderSettings es;
    es.totalSamples     = totalFrames;
    es.compressionLevel = 0;

    REQUIRE(enc.open(tmp.path, es));
    REQUIRE(enc.writeSamples(silence(totalFrames)));
    REQUIRE(enc.finalize());

    REQUIRE(std::filesystem::exists(tmp.path));
    REQUIRE(std::filesystem::file_size(tmp.path) > 0);
}

// ---------------------------------------------------------------------------
// cdBytesToSamples utility
// ---------------------------------------------------------------------------

TEST_CASE("cdBytesToSamples — LE int16 bytes map to int32 samples", "[flacenc]") {
    // Encode value 0x0102 as little-endian: bytes [0x02, 0x01]
    // Expected int16 = 0x0102 = 258
    uint8_t bytes[4] = { 0x02, 0x01,   // left channel
                         0x02, 0x01 }; // right channel (same value)
    std::vector<int32_t> out;
    cdBytesToSamples(bytes, 4, out);

    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == 258);  // left
    REQUIRE(out[1] == 258);  // right
}

TEST_CASE("cdBytesToSamples — negative values sign-extend correctly", "[flacenc]") {
    // 0xFF 0xFF = 0xFFFF as uint16 = -1 as int16
    uint8_t bytes[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    std::vector<int32_t> out;
    cdBytesToSamples(bytes, 4, out);

    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == -1);
    REQUIRE(out[1] == -1);
}

TEST_CASE("cdBytesToSamples — zero bytes produce zero samples", "[flacenc]") {
    std::vector<uint8_t> bytes(2352, 0);
    std::vector<int32_t> out;
    cdBytesToSamples(bytes.data(), bytes.size(), out);

    REQUIRE(out.size() == 1176);
    for (auto s : out) REQUIRE(s == 0);
}
