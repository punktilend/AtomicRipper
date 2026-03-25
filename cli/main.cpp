#include <core/drive/DriveEnumerator.hpp>
#include <core/drive/TOC.hpp>
#include <core/metadata/DiscId.hpp>
#include <core/rip/RipEngine.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace atomicripper;

// ---------------------------------------------------------------------------
// WAV file writer — no external dependencies
// ---------------------------------------------------------------------------

namespace {

void writeLE16(FILE* fp, uint16_t v) {
    uint8_t b[2] = { uint8_t(v & 0xFF), uint8_t(v >> 8) };
    fwrite(b, 1, 2, fp);
}

void writeLE32(FILE* fp, uint32_t v) {
    uint8_t b[4] = {
        uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF),
        uint8_t((v >> 16) & 0xFF), uint8_t((v >> 24) & 0xFF)
    };
    fwrite(b, 1, 4, fp);
}

// Write a standard 44-byte RIFF/PCM WAV header for 44100 Hz, stereo, 16-bit.
// Call with dataBytes = 0 first, then fseek back and call again once you know the size.
void writeWavHeader(FILE* fp, uint32_t dataBytes) {
    fwrite("RIFF", 1, 4, fp);
    writeLE32(fp, dataBytes + 36);   // file size - 8
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    writeLE32(fp, 16);               // fmt chunk size
    writeLE16(fp, 1);                // PCM
    writeLE16(fp, 2);                // stereo
    writeLE32(fp, 44100);            // sample rate
    writeLE32(fp, 176400);           // byte rate (44100 * 4)
    writeLE16(fp, 4);                // block align
    writeLE16(fp, 16);               // bits per sample
    fwrite("data", 1, 4, fp);
    writeLE32(fp, dataBytes);        // data chunk size
}

} // namespace

// ---------------------------------------------------------------------------
// TOC display
// ---------------------------------------------------------------------------

static void printTOC(const drive::TOC& toc) {
    printf("  Tracks : %d-%d\n", toc.firstTrack, toc.lastTrack);
    printf("  Lead-out: sector %u  (%.1f sec)\n",
           toc.leadOutLBA, toc.durationSeconds());

    for (const auto& t : toc.tracks) {
        double lenSec = static_cast<double>(t.sectorCount) / 75.0;
        printf("  Track %02d  %-5s  LBA %-6u  %5.1f s  (%u sectors)\n",
               t.number,
               t.isAudio ? "AUDIO" : "DATA ",
               t.lba,
               lenSec,
               t.sectorCount);
    }

    std::string discId = metadata::DiscId::calculate(toc);
    if (!discId.empty())
        printf("  MusicBrainz Disc ID: %s\n", discId.c_str());
}

// ---------------------------------------------------------------------------
// Drive list
// ---------------------------------------------------------------------------

static void listDrives() {
    auto drives = drive::DriveEnumerator::enumerate();

    if (drives.empty()) {
        printf("No optical drives found.\n");
        return;
    }

    printf("Found %zu optical drive(s):\n\n", drives.size());
    for (const auto& drv : drives) {
        printf("[%s]  %s\n", drv.path().c_str(), drv.description().c_str());

        switch (drv.status()) {
        case drive::DriveStatus::Ready:
            printf("  Status: READY\n");
            if (auto toc = drv.readTOC())
                printTOC(*toc);
            break;
        case drive::DriveStatus::Empty:
            printf("  Status: EMPTY (no disc)\n");
            break;
        case drive::DriveStatus::NotReady:
            printf("  Status: NOT READY\n");
            break;
        case drive::DriveStatus::Error:
            printf("  Status: ERROR\n");
            break;
        }
        printf("\n");
    }
}

// ---------------------------------------------------------------------------
// Rip command
// ---------------------------------------------------------------------------

static int doRip(const std::string& drivePath, const std::string& outputDir) {
    // Validate output directory
    std::filesystem::path outPath(outputDir);
    if (!outputDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(outPath, ec);
        if (ec) {
            printf("Error: cannot create output directory '%s': %s\n",
                   outputDir.c_str(), ec.message().c_str());
            return 1;
        }
    } else {
        outPath = std::filesystem::current_path();
    }

    // Read TOC
    drive::Drive drv(drivePath, drivePath);
    if (!drv.isReady()) {
        printf("Drive %s is not ready (no disc or drive error).\n", drivePath.c_str());
        return 1;
    }
    auto toc = drv.readTOC();
    if (!toc || !toc->isValid()) {
        printf("Failed to read TOC from %s.\n", drivePath.c_str());
        return 1;
    }

    printf("Drive : %s\n", drivePath.c_str());
    printf("Disc  : %d tracks, %.0f sec\n",
           toc->audioTrackCount(), toc->durationSeconds());
    printf("Output: %s\n\n", outPath.string().c_str());

    // Open rip engine
    rip::RipSettings settings;
    settings.mode       = rip::RipMode::Secure;
    settings.maxRetries = 16;
    settings.minMatches = 2;
    settings.useC2Errors = true;
    settings.driveOffset = 0;  // user should set this after AccurateRip detection (Phase 4)

    rip::RipEngine engine(drivePath, settings);
    if (!engine.open()) {
        printf("Error: could not open drive %s for SCSI access.\n", drivePath.c_str());
        printf("  (Try running as Administrator)\n");
        return 1;
    }

    engine.probeCapabilities();
    printf("C2 error support: %s\n\n", engine.supportsC2() ? "yes" : "no");

    int audioTracksDone = 0;
    int totalErrors     = 0;

    for (const auto& track : toc->tracks) {
        if (!track.isAudio) {
            printf("Track %02d: DATA — skipping\n", track.number);
            continue;
        }

        const int totalTracks = toc->audioTrackCount();
        printf("Ripping track %02d/%02d  (%u sectors, %.1f sec)\n",
               track.number, totalTracks,
               track.sectorCount,
               static_cast<double>(track.sectorCount) / 75.0);

        // Progress callback — overwrites the same line
        auto onProgress = [](const rip::RipProgress& p) {
            const int barWidth = 30;
            float     frac     = (p.totalSectors > 0)
                ? static_cast<float>(p.currentSector) / static_cast<float>(p.totalSectors)
                : 0.0f;
            int filled = static_cast<int>(frac * barWidth);

            printf("\r  [");
            for (int i = 0; i < barWidth; ++i)
                putchar(i < filled ? '=' : (i == filled ? '>' : ' '));
            printf("] %3.0f%%  %4.1f\xC3\x97  %d retries   ",
                   frac * 100.0f, p.speedX, p.totalRetries);
            fflush(stdout);
        };

        auto result = engine.ripTrack(track, track.number, onProgress);
        printf("\n");  // end progress line

        if (result.cancelled) {
            printf("  Cancelled.\n");
            return 1;
        }
        if (!result.ok) {
            printf("  ERROR: rip failed for track %02d\n", track.number);
            ++totalErrors;
            continue;
        }

        // Count low-confidence (suspect) sectors
        int suspectSectors = 0;
        int c2Sectors      = 0;
        for (const auto& sr : result.sectors) {
            if (sr.confidence == 0)   ++suspectSectors;
            if (sr.hasC2Errors)       ++c2Sectors;
        }

        printf("  CRC32: %08X  |  %zu sectors  |  confidence: %s  |  C2 errors: %d\n",
               result.crc32,
               result.sectors.size(),
               suspectSectors == 0 ? "OK" : "SUSPECT",
               c2Sectors);

        if (suspectSectors > 0)
            printf("  WARNING: %d sector(s) could not reach read consensus\n", suspectSectors);

        // Write WAV file
        char filename[64];
        snprintf(filename, sizeof(filename), "track%02d.wav", track.number);
        auto wavPath = (outPath / filename).string();

        FILE* fp = fopen(wavPath.c_str(), "wb");
        if (!fp) {
            printf("  ERROR: cannot write %s\n", wavPath.c_str());
            ++totalErrors;
            continue;
        }

        const uint32_t dataBytes = static_cast<uint32_t>(result.data.size());
        writeWavHeader(fp, dataBytes);
        fwrite(result.data.data(), 1, result.data.size(), fp);

        // Patch header with actual data size
        fseek(fp, 0, SEEK_SET);
        writeWavHeader(fp, dataBytes);
        fclose(fp);

        printf("  -> %s\n\n", wavPath.c_str());
        ++audioTracksDone;
    }

    printf("Done. %d track(s) ripped", audioTracksDone);
    if (totalErrors > 0)
        printf(", %d error(s)", totalErrors);
    printf(".\n");

    return totalErrors > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void printUsage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  (none)                  List all optical drives and disc info\n");
    printf("  --list-drives           Same as above\n");
    printf("  --toc <drive>           Read and display TOC (e.g. D:)\n");
    printf("  --rip <drive> [outdir]  Rip all audio tracks to WAV files\n");
    printf("  --help                  Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --toc D:\n", prog);
    printf("  %s --rip D:\n", prog);
    printf("  %s --rip D: C:\\Music\\MyDisc\n", prog);
}

int main(int argc, char* argv[]) {
    printf("AtomicRipper v0.2.0\n");
    printf("===================\n\n");

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "--list-drives") == 0)) {
        listDrives();
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printUsage(argv[0]);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--toc") == 0) {
        if (argc < 3) { printf("--toc requires a drive letter\n"); return 1; }
        std::string path = argv[2];
        if (path.size() == 1) path += ':';
        if (path.back() == '\\') path.pop_back();

        drive::Drive drv(path, path);
        if (!drv.isReady()) {
            printf("Drive %s is not ready.\n", path.c_str());
            return 1;
        }
        auto toc = drv.readTOC();
        if (!toc) { printf("Failed to read TOC.\n"); return 1; }
        printTOC(*toc);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--rip") == 0) {
        if (argc < 3) { printf("--rip requires a drive letter\n"); return 1; }
        std::string path = argv[2];
        if (path.size() == 1) path += ':';
        if (path.back() == '\\') path.pop_back();

        std::string outDir = (argc >= 4) ? argv[3] : "";
        return doRip(path, outDir);
    }

    printUsage(argv[0]);
    return 1;
}
