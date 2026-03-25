#include <core/drive/Drive.hpp>
#include <core/drive/DriveEnumerator.hpp>
#include <core/drive/TOC.hpp>
#include <core/metadata/DiscId.hpp>
#include <core/pipeline/Pipeline.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace atomicripper;

// ---------------------------------------------------------------------------
// TOC display
// ---------------------------------------------------------------------------
static void printTOC(const drive::TOC& toc) {
    printf("  Tracks : %d-%d\n", toc.firstTrack, toc.lastTrack);
    printf("  Lead-out: sector %u  (%.1f sec)\n", toc.leadOutLBA, toc.durationSeconds());
    for (const auto& t : toc.tracks)
        printf("  Track %02d  %-5s  LBA %-6u  %5.1f s  (%u sectors)\n",
               t.number, t.isAudio ? "AUDIO" : "DATA ",
               t.lba, static_cast<double>(t.sectorCount)/75.0, t.sectorCount);
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
        case drive::DriveStatus::Empty:    printf("  Status: EMPTY\n");     break;
        case drive::DriveStatus::NotReady: printf("  Status: NOT READY\n"); break;
        case drive::DriveStatus::Error:    printf("  Status: ERROR\n");     break;
        }
        printf("\n");
    }
}

// ---------------------------------------------------------------------------
// Rip command — drives the Pipeline with console callbacks
// ---------------------------------------------------------------------------
static int doRip(const std::string& drivePath,
                 const std::string& outputDir,
                 encode::Format     format,
                 int                driveOffset,
                 bool               detectOffset = false,
                 bool               eject        = false) {
    std::filesystem::path outPath = outputDir.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(outputDir);

    // Build config
    pipeline::PipelineConfig cfg;
    cfg.outputDir                        = outPath;
    cfg.format                           = format;
    cfg.ripSettings.mode                 = rip::RipMode::Secure;
    cfg.ripSettings.maxRetries           = 16;
    cfg.ripSettings.minMatches           = 2;
    cfg.ripSettings.useC2Errors          = true;
    cfg.ripSettings.driveOffset          = driveOffset;
    cfg.encoderSettings.compressionLevel = 8;
    cfg.fetchMetadata                    = true;
    cfg.embedCoverArt                    = (format == encode::Format::FLAC);
    cfg.verifyAccurateRip                = true;
    cfg.autoDetectOffset                 = detectOffset;
    cfg.writeTags                        = (format == encode::Format::FLAC);
    cfg.ejectWhenDone                    = eject;
    cfg.autoSelectRelease                = false;

    // State shared across callbacks
    int  exitCode        = 0;
    int  totalAudioTracks = 0;

    // pipePtr is set after Pipeline is constructed; callbacks are only called
    // from the worker thread, which starts after pipePtr is assigned.
    pipeline::Pipeline* pipePtr = nullptr;

    pipeline::PipelineCallbacks cb;

    cb.onTocRead = [&](const drive::TOC& toc) {
        totalAudioTracks = toc.audioTrackCount();
        const char* fmtName = (format == encode::Format::WAV) ? "WAV" : "FLAC";
        printf("Drive  : %s\n", drivePath.c_str());
        printf("Disc   : %d tracks, %.0f sec\n", totalAudioTracks, toc.durationSeconds());
        printf("Format : %s\n", fmtName);
        if (driveOffset != 0) printf("Offset : %+d samples\n", driveOffset);
        printf("Output : %s\n\n", outPath.string().c_str());
    };

    cb.onTrackStart = [](int trackNumber, int total, uint32_t sectors) {
        printf("Ripping track %02d/%d  (%u sectors, %.1f sec)\n",
               trackNumber, total, sectors, static_cast<double>(sectors)/75.0);
        fflush(stdout);
    };

    cb.onTrackProgress = [](const rip::RipProgress& p) {
        const int   W    = 28;
        const float frac = p.totalSectors
            ? static_cast<float>(p.currentSector)/p.totalSectors : 0.0f;
        const int   fill = static_cast<int>(frac * W);
        printf("\r  [");
        for (int i = 0; i < W; ++i)
            putchar(i < fill ? '=' : (i == fill ? '>' : ' '));
        printf("] %3.0f%%  %4.1fx  %d retries   ", frac*100.0f, p.speedX, p.totalRetries);
        fflush(stdout);
    };

    cb.onTrackDone = [&](const pipeline::TrackDoneInfo& info) {
        printf("\n");
        if (!info.ok) {
            printf("  ERROR: rip failed for track %02d\n\n", info.trackNumber);
            exitCode = 1;
            return;
        }
        printf("  CRC32: %08X  |  rip: %s  |  C2: %d sector(s)\n",
               info.crc32,
               info.suspectSectors == 0 ? "OK" : "SUSPECT",
               info.c2Sectors);
        if (info.suspectSectors > 0)
            printf("  WARNING: %d sector(s) could not reach read consensus\n", info.suspectSectors);
        printf("  -> %s\n\n", info.outputPath.string().c_str());
    };

    cb.onVerifyDone = [&](const verify::ArDiscResult& ar) {
        printf("AccurateRip verification...\n");
        if (!ar.lookupOk) {
            printf("  Lookup failed: %s\n\n", ar.error.c_str());
            return;
        }
        if (!ar.error.empty()) { printf("  %s\n\n", ar.error.c_str()); return; }
        printf("  DB pressings found: %d\n", ar.dbEntries);
        int matched = 0;
        for (const auto& tr : ar.tracks) {
            int conf = tr.confidenceV2 > 0 ? tr.confidenceV2 : tr.confidenceV1;
            printf("  Track %02d: %s  conf=%-3d  CRCv1=%08X  CRCv2=%08X\n",
                   tr.trackNumber,
                   tr.matched ? "OK      " : "NO MATCH",
                   conf, tr.checksumV1, tr.checksumV2);
            if (tr.matched) ++matched;
        }
        printf("  %d/%d tracks verified accurately\n", matched, totalAudioTracks);
        if (matched < totalAudioTracks && driveOffset == 0)
            printf("  Hint: if no tracks match, try --offset to set your drive's read offset.\n");
        printf("\n");
    };

    // onMetadataReady: called by worker thread; must call pipePtr->selectRelease()
    cb.onMetadataReady = [&pipePtr](const metadata::MbResult& mb) {
        printf("MusicBrainz lookup found %zu release(s):\n\n", mb.releases.size());
        for (size_t i = 0; i < mb.releases.size(); ++i) {
            const auto& r = mb.releases[i];
            printf("  [%zu] %s — %s (%s)%s\n",
                   i + 1,
                   r.title.c_str(), r.artist.c_str(),
                   r.date.empty() ? "?" : r.date.substr(0, 4).c_str(),
                   r.country.empty() ? "" : (" [" + r.country + "]").c_str());
        }
        int selectedIdx = 0;
        if (mb.releases.size() > 1) {
            printf("\n  Select [1-%zu] (Enter = #1): ", mb.releases.size());
            fflush(stdout);
            char buf[16] = {};
            if (fgets(buf, sizeof(buf), stdin)) {
                int choice = std::atoi(buf);
                if (choice >= 1 && choice <= static_cast<int>(mb.releases.size()))
                    selectedIdx = choice - 1;
            }
        }
        const auto& rel = mb.releases[static_cast<size_t>(selectedIdx)];
        printf("\n  Using: %s — %s\n\n", rel.title.c_str(), rel.artist.c_str());
        pipePtr->selectRelease(selectedIdx);
    };

    cb.onOffsetDetected = [](const verify::ArOffsetResult& r) {
        if (!r.found) {
            printf("  Offset detection: %s\n\n",
                   r.error.empty() ? "no match found" : r.error.c_str());
            return;
        }
        printf("  Detected drive offset: %+d samples  "
               "(conf=%d, %d track(s) matched)\n",
               r.sampleOffset, r.confidence, r.tracksMatched);
        printf("  Re-rip with: --offset %d\n\n", r.sampleOffset);
    };

    cb.onTagsDone = [](int tagged) {
        printf("  %d track(s) tagged.\n\n", tagged);
    };

    cb.onError = [&](const std::string& msg) {
        printf("\nERROR: %s\n", msg.c_str());
        exitCode = 1;
    };

    cb.onCancelled = []() { printf("\nCancelled.\n"); };
    cb.onComplete  = []() { printf("Done.\n"); };

    // Construct the pipeline, then set pipePtr before calling start().
    // The worker thread only fires after start(), so pipePtr is always valid.
    pipeline::Pipeline pipe(std::move(cfg), std::move(cb));
    pipePtr = &pipe;

    pipe.start(drivePath);
    pipe.waitForCompletion();
    return exitCode;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
static void printUsage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  (none)                              List all optical drives\n");
    printf("  --list-drives                       Same as above\n");
    printf("  --toc <drive>                       Read and display TOC (e.g. D:)\n");
    printf("  --rip <drive> [outdir]              Rip all audio tracks to FLAC\n");
    printf("  --rip <drive> [outdir] --wav        Rip to WAV instead\n");
    printf("  --rip <drive> [outdir] --offset N   Set drive read offset in samples\n");
    printf("  --rip <drive> [outdir] --detect-offset  Auto-detect offset via AccurateRip\n");
    printf("  --rip <drive> [outdir] --eject          Eject disc when rip completes\n");
    printf("  --help                              Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --toc D:\n", prog);
    printf("  %s --rip D:\n", prog);
    printf("  %s --rip D: \"C:\\Music\\MyDisc\"\n", prog);
    printf("  %s --rip D: --offset 30\n", prog);
}

int main(int argc, char* argv[]) {
    printf("AtomicRipper v0.6.0\n");
    printf("===================\n\n");

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "--list-drives") == 0)) {
        listDrives(); return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printUsage(argv[0]); return 0;
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
        printTOC(*toc); return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--rip") == 0) {
        if (argc < 3) { printf("--rip requires a drive letter\n"); return 1; }
        std::string path = argv[2];
        if (path.size() == 1) path += ':';
        if (!path.empty() && path.back() == '\\') path.pop_back();

        std::string    outDir;
        encode::Format fmt          = encode::Format::FLAC;
        int            offset       = 0;
        bool           detectOff    = false;
        bool           ejectAfter   = false;

        for (int i = 3; i < argc; ++i) {
            if      (strcmp(argv[i], "--wav") == 0)
                fmt = encode::Format::WAV;
            else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc)
                offset = std::atoi(argv[++i]);
            else if (strcmp(argv[i], "--detect-offset") == 0)
                detectOff = true;
            else if (strcmp(argv[i], "--eject") == 0)
                ejectAfter = true;
            else if (outDir.empty())
                outDir = argv[i];
        }
        return doRip(path, outDir, fmt, offset, detectOff, ejectAfter);
    }

    printUsage(argv[0]); return 1;
}
