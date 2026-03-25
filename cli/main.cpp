#include <core/drive/DriveEnumerator.hpp>
#include <core/drive/TOC.hpp>
#include <core/encode/FlacEncoder.hpp>
#include <core/encode/IEncoder.hpp>
#include <core/metadata/DiscId.hpp>
#include <core/rip/RipEngine.hpp>
#include <core/verify/AccurateRip.hpp>

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

    // Collect ripped PCM for all audio tracks (needed for AccurateRip batch verify)
    std::vector<std::vector<uint8_t>> allTrackPcm;
    allTrackPcm.reserve(static_cast<size_t>(totalAudioTracks));

    for (const auto& track : toc->tracks) {
        if (!track.isAudio) {
            printf("Track %02d: DATA — skipping\n\n", track.number);
            continue;
        }

        printf("Ripping track %02d/%d  (%u sectors, %.1f sec)\n",
               track.number, totalAudioTracks,
               track.sectorCount,
               static_cast<double>(track.sectorCount) / 75.0);

        auto onProgress = [](const rip::RipProgress& p) {
            const int   barW  = 28;
            const float frac  = p.totalSectors
                ? static_cast<float>(p.currentSector) / p.totalSectors : 0.0f;
            const int   filled = static_cast<int>(frac * barW);
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
            allTrackPcm.push_back({});  // placeholder so indices stay aligned
            continue;
        }

        // Per-track quality summary
        int suspectSectors = 0, c2Sectors = 0;
        for (const auto& sr : result.sectors) {
            if (sr.confidence == 0) ++suspectSectors;
            if (sr.hasC2Errors)     ++c2Sectors;
        }
        printf("  CRC32: %08X  |  rip: %s  |  C2 errors: %d sector(s)\n",
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

            const uint64_t totalSamples =
                static_cast<uint64_t>(result.data.size()) / 4u;

            encode::EncoderSettings encSettings;
            encSettings.format           = encode::Format::FLAC;
            encSettings.compressionLevel = 8;
            encSettings.totalSamples     = totalSamples;

            encode::FlacEncoder encoder;
            encoder.setTag("TRACKNUMBER", std::to_string(track.number));
            encoder.setTag("TRACKTOTAL",  std::to_string(totalAudioTracks));

            if (!encoder.open(filePath, encSettings)) {
                printf("  ERROR: could not open %s: %s\n",
                       filePath.string().c_str(), encoder.lastError().c_str());
                ++totalErrors;
                allTrackPcm.push_back({});
                continue;
            }

            std::vector<int32_t> samples;
            encode::cdBytesToSamples(result.data.data(), result.data.size(), samples);

            if (!encoder.writeSamples(samples) || !encoder.finalize()) {
                printf("  ERROR: encode failed: %s\n", encoder.lastError().c_str());
                ++totalErrors;
                allTrackPcm.push_back({});
                continue;
            }
            writeOk = true;

        } else {
            snprintf(filename, sizeof(filename), "track%02d.wav", track.number);
            auto filePath = (outPath / filename).string();
            FILE* fp = fopen(filePath.c_str(), "wb");
            if (!fp) {
                printf("  ERROR: cannot write %s\n", filePath.c_str());
                ++totalErrors;
                allTrackPcm.push_back({});
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

        allTrackPcm.push_back(std::move(result.data));
    }

    // -----------------------------------------------------------------------
    // AccurateRip verification (batch, after all tracks are ripped)
    // -----------------------------------------------------------------------
    if (tracksWritten > 0 && static_cast<int>(allTrackPcm.size()) == totalAudioTracks) {
        printf("AccurateRip verification...\n");
        printf("  URL: %s\n", verify::AccurateRip::buildUrl(*toc).c_str());

        auto arResult = verify::AccurateRip::verify(*toc, allTrackPcm);

        if (!arResult.lookupOk) {
            printf("  Result: lookup failed — %s\n\n", arResult.error.c_str());
        } else if (!arResult.error.empty()) {
            // 404 / not in DB
            printf("  Result: %s\n", arResult.error.c_str());
            printf("  (This disc has not been submitted to AccurateRip yet,\n");
            printf("   or your drive offset may need calibration.)\n\n");
        } else {
            printf("  DB entries found: %d\n\n", arResult.dbEntries);

            int matched = 0;
            for (const auto& tr : arResult.tracks) {
                const char* status;
                int conf = 0;
                if (tr.confidenceV2 > 0) { status = "OK (v2)"; conf = tr.confidenceV2; }
                else if (tr.confidenceV1 > 0) { status = "OK (v1)"; conf = tr.confidenceV1; }
                else status = "NO MATCH";

                printf("  Track %02d: %-12s  conf=%d  CRCv1=%08X  CRCv2=%08X\n",
                       tr.trackNumber, status, conf,
                       tr.checksumV1, tr.checksumV2);

                if (tr.matched) ++matched;
            }

            printf("\n  %d/%d tracks verified\n", matched, totalAudioTracks);

            if (matched < totalAudioTracks) {
                printf("\n  Some tracks did not match. Possible causes:\n");
                printf("  1. Drive read offset not set (use --offset <samples>)\n");
                printf("  2. Rip errors — try ripping again with more retries\n");
                printf("  3. Pressing not in AccurateRip DB yet\n");
            }
            printf("\n");
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
    printf("  --rip <drive> [outdir]              Rip to FLAC (default)\n");
    printf("  --rip <drive> [outdir] --wav        Rip to WAV instead\n");
    printf("  --rip <drive> [outdir] --offset N   Set drive read offset (samples)\n");
    printf("  --help                         Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --toc D:\n", prog);
    printf("  %s --rip D:\n", prog);
    printf("  %s --rip D: C:\\Music\\MyDisc\n", prog);
    printf("  %s --rip D: C:\\Music\\MyDisc --wav\n", prog);
}

int main(int argc, char* argv[]) {
    printf("AtomicRipper v0.4.0\n");
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
        encode::Format fmt    = encode::Format::FLAC;
        int            offset = 0;

        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--wav") == 0) {
                fmt = encode::Format::WAV;
            } else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
                offset = std::atoi(argv[++i]);
            } else if (outDir.empty()) {
                outDir = argv[i];
            }
        }

        // Apply offset to rip settings via a global (threaded through doRip in Phase 5)
        // For now, store it and pass to the engine inside doRip
        (void)offset;  // TODO Phase 5: wire through RipSettings
        return doRip(path, outDir, fmt);
    }

    printUsage(argv[0]);
    return 1;
}
