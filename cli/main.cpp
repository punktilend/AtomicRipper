#include <core/drive/DriveEnumerator.hpp>
#include <core/drive/TOC.hpp>
#include <core/encode/FlacEncoder.hpp>
#include <core/encode/IEncoder.hpp>
#include <core/metadata/DiscId.hpp>
#include <core/rip/RipEngine.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace atomicripper;

// ---------------------------------------------------------------------------
// WAV fallback writer — no external dependencies
// ---------------------------------------------------------------------------

namespace {

void writeLE16(FILE* fp, uint16_t v) {
    uint8_t b[2] = { uint8_t(v & 0xFF), uint8_t(v >> 8) };
    fwrite(b, 1, 2, fp);
}
void writeLE32(FILE* fp, uint32_t v) {
    uint8_t b[4] = {
        uint8_t( v        & 0xFF), uint8_t((v >>  8) & 0xFF),
        uint8_t((v >> 16) & 0xFF), uint8_t((v >> 24) & 0xFF) };
    fwrite(b, 1, 4, fp);
}
void writeWavHeader(FILE* fp, uint32_t dataBytes) {
    fwrite("RIFF", 1, 4, fp);  writeLE32(fp, dataBytes + 36);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);  writeLE32(fp, 16);
    writeLE16(fp, 1);           writeLE16(fp, 2);         // PCM, stereo
    writeLE32(fp, 44100);       writeLE32(fp, 176400);    // sampleRate, byteRate
    writeLE16(fp, 4);           writeLE16(fp, 16);        // blockAlign, bitsPerSample
    fwrite("data", 1, 4, fp);  writeLE32(fp, dataBytes);
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
        printf("  Track %02d  %-5s  LBA %-6u  %5.1f s  (%u sectors)\n",
               t.number, t.isAudio ? "AUDIO" : "DATA ",
               t.lba,
               static_cast<double>(t.sectorCount) / 75.0,
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
    if (drives.empty()) { printf("No optical drives found.\n"); return; }

    printf("Found %zu optical drive(s):\n\n", drives.size());
    for (const auto& drv : drives) {
        printf("[%s]  %s\n", drv.path().c_str(), drv.description().c_str());
        switch (drv.status()) {
        case drive::DriveStatus::Ready:
            printf("  Status: READY\n");
            if (auto toc = drv.readTOC()) printTOC(*toc);
            break;
        case drive::DriveStatus::Empty:   printf("  Status: EMPTY\n");     break;
        case drive::DriveStatus::NotReady: printf("  Status: NOT READY\n"); break;
        case drive::DriveStatus::Error:    printf("  Status: ERROR\n");     break;
        }
        printf("\n");
    }
}

// ---------------------------------------------------------------------------
// Rip command
// ---------------------------------------------------------------------------

static int doRip(const std::string& drivePath,
                 const std::string& outputDir,
                 encode::Format     format) {
    // Resolve output directory
    std::filesystem::path outPath = outputDir.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(outputDir);

    std::error_code ec;
    std::filesystem::create_directories(outPath, ec);
    if (ec) {
        printf("Error: cannot create output directory '%s': %s\n",
               outPath.string().c_str(), ec.message().c_str());
        return 1;
    }

    // Read TOC
    drive::Drive drv(drivePath, drivePath);
    if (!drv.isReady()) {
        printf("Drive %s is not ready (no disc or drive error).\n",
               drivePath.c_str());
        return 1;
    }
    auto toc = drv.readTOC();
    if (!toc || !toc->isValid()) {
        printf("Failed to read TOC from %s.\n", drivePath.c_str());
        return 1;
    }

    const char* fmtName = (format == encode::Format::WAV) ? "WAV" : "FLAC";
    printf("Drive : %s\n", drivePath.c_str());
    printf("Disc  : %d tracks, %.0f sec\n",
           toc->audioTrackCount(), toc->durationSeconds());
    printf("Format: %s\n", fmtName);
    printf("Output: %s\n\n", outPath.string().c_str());

    // Open rip engine
    rip::RipSettings ripSettings;
    ripSettings.mode        = rip::RipMode::Secure;
    ripSettings.maxRetries  = 16;
    ripSettings.minMatches  = 2;
    ripSettings.useC2Errors = true;
    ripSettings.driveOffset = 0;  // set after AccurateRip detection (Phase 4)

    rip::RipEngine engine(drivePath, ripSettings);
    if (!engine.open()) {
        printf("Error: could not open drive %s for SCSI access.\n",
               drivePath.c_str());
        printf("  (Try running as Administrator)\n");
        return 1;
    }

    engine.probeCapabilities();
    printf("C2 error support : %s\n\n", engine.supportsC2() ? "yes" : "no");

    const int totalAudioTracks = toc->audioTrackCount();
    int       tracksWritten    = 0;
    int       totalErrors      = 0;

    for (const auto& track : toc->tracks) {
        if (!track.isAudio) {
            printf("Track %02d: DATA — skipping\n\n", track.number);
            continue;
        }

        printf("Ripping track %02d/%d  (%u sectors, %.1f sec)\n",
               track.number, totalAudioTracks,
               track.sectorCount,
               static_cast<double>(track.sectorCount) / 75.0);

        // Live progress bar
        auto onProgress = [](const rip::RipProgress& p) {
            const int  barW  = 28;
            const float frac = p.totalSectors
                ? static_cast<float>(p.currentSector) / p.totalSectors
                : 0.0f;
            const int filled = static_cast<int>(frac * barW);

            printf("\r  [");
            for (int i = 0; i < barW; ++i)
                putchar(i < filled ? '=' : (i == filled ? '>' : ' '));
            printf("] %3.0f%%  %4.1fx  %d retries   ",
                   frac * 100.0f, p.speedX, p.totalRetries);
            fflush(stdout);
        };

        auto result = engine.ripTrack(track, track.number, onProgress);
        printf("\n");

        if (result.cancelled) { printf("  Cancelled.\n"); return 1; }
        if (!result.ok) {
            printf("  ERROR: rip failed for track %02d\n\n", track.number);
            ++totalErrors;
            continue;
        }

        // Quality summary
        int suspectSectors = 0, c2Sectors = 0;
        for (const auto& sr : result.sectors) {
            if (sr.confidence == 0) ++suspectSectors;
            if (sr.hasC2Errors)     ++c2Sectors;
        }
        printf("  CRC32: %08X  |  confidence: %s  |  C2 errors: %d sectors\n",
               result.crc32,
               suspectSectors == 0 ? "OK" : "SUSPECT",
               c2Sectors);
        if (suspectSectors > 0)
            printf("  WARNING: %d sector(s) could not reach read consensus\n",
                   suspectSectors);

        // Write output file
        char filename[64];
        bool writeOk = false;

        if (format == encode::Format::FLAC) {
            snprintf(filename, sizeof(filename), "track%02d.flac", track.number);
            auto filePath = outPath / filename;

            // Samples = byteCount / 2 (16-bit each), frames = samples / 2 (stereo)
            const uint64_t totalSamples =
                static_cast<uint64_t>(result.data.size()) / 4u;  // /4 = stereo frames

            encode::EncoderSettings encSettings;
            encSettings.format           = encode::Format::FLAC;
            encSettings.compressionLevel = 8;
            encSettings.totalSamples     = totalSamples;

            encode::FlacEncoder encoder;
            encoder.setTag("TRACKNUMBER", std::to_string(track.number));
            encoder.setTag("TRACKTOTAL",  std::to_string(totalAudioTracks));
            // TITLE / ARTIST / ALBUM filled in Phase 5 (MusicBrainz)

            if (!encoder.open(filePath, encSettings)) {
                printf("  ERROR: could not open %s: %s\n",
                       filePath.string().c_str(), encoder.lastError().c_str());
                ++totalErrors;
                continue;
            }

            // Convert raw CD bytes to int32_t samples and feed to encoder
            std::vector<int32_t> samples;
            encode::cdBytesToSamples(result.data.data(), result.data.size(), samples);

            if (!encoder.writeSamples(samples)) {
                printf("  ERROR: write failed: %s\n", encoder.lastError().c_str());
                ++totalErrors;
                continue;
            }

            if (!encoder.finalize()) {
                printf("  ERROR: finalize failed: %s\n", encoder.lastError().c_str());
                ++totalErrors;
                continue;
            }

            writeOk = true;

        } else {
            // WAV fallback
            snprintf(filename, sizeof(filename), "track%02d.wav", track.number);
            auto filePath = (outPath / filename).string();
            FILE* fp = fopen(filePath.c_str(), "wb");
            if (!fp) {
                printf("  ERROR: cannot write %s\n", filePath.c_str());
                ++totalErrors;
                continue;
            }
            const uint32_t dataBytes = static_cast<uint32_t>(result.data.size());
            writeWavHeader(fp, dataBytes);
            fwrite(result.data.data(), 1, result.data.size(), fp);
            fseek(fp, 0, SEEK_SET);
            writeWavHeader(fp, dataBytes);
            fclose(fp);
            writeOk = true;
        }

        if (writeOk) {
            printf("  -> %s\n\n", (outPath / filename).string().c_str());
            ++tracksWritten;
        }
    }

    printf("Done. %d track(s) written", tracksWritten);
    if (totalErrors > 0) printf(", %d error(s)", totalErrors);
    printf(".\n");
    return totalErrors > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void printUsage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  (none)                         List all optical drives\n");
    printf("  --list-drives                  Same as above\n");
    printf("  --toc <drive>                  Read and display TOC (e.g. D:)\n");
    printf("  --rip <drive> [outdir]         Rip to FLAC (default)\n");
    printf("  --rip <drive> [outdir] --wav   Rip to WAV instead\n");
    printf("  --help                         Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --toc D:\n", prog);
    printf("  %s --rip D:\n", prog);
    printf("  %s --rip D: C:\\Music\\MyDisc\n", prog);
    printf("  %s --rip D: C:\\Music\\MyDisc --wav\n", prog);
}

int main(int argc, char* argv[]) {
    printf("AtomicRipper v0.3.0\n");
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
        if (!path.empty() && path.back() == '\\') path.pop_back();
        drive::Drive drv(path, path);
        if (!drv.isReady()) { printf("Drive %s is not ready.\n", path.c_str()); return 1; }
        auto toc = drv.readTOC();
        if (!toc) { printf("Failed to read TOC.\n"); return 1; }
        printTOC(*toc);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--rip") == 0) {
        if (argc < 3) { printf("--rip requires a drive letter\n"); return 1; }
        std::string path = argv[2];
        if (path.size() == 1) path += ':';
        if (!path.empty() && path.back() == '\\') path.pop_back();

        std::string    outDir;
        encode::Format fmt = encode::Format::FLAC;

        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--wav") == 0)
                fmt = encode::Format::WAV;
            else if (outDir.empty())
                outDir = argv[i];
        }

        return doRip(path, outDir, fmt);
    }

    printUsage(argv[0]);
    return 1;
}
