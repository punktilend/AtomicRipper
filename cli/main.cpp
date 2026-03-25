#include <core/drive/DriveEnumerator.hpp>
#include <core/drive/TOC.hpp>
#include <core/metadata/DiscId.hpp>

#include <cstdio>
#include <cstring>
#include <string>

using namespace atomicripper;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void printTOC(const drive::TOC& toc) {
    printf("  Tracks : %d–%d\n", toc.firstTrack, toc.lastTrack);
    printf("  Lead-out: sector %u  (%.1f sec)\n",
           toc.leadOutLBA, toc.durationSeconds());

    for (const auto& t : toc.tracks) {
        double startSec = static_cast<double>(t.lba) / 75.0;
        double lenSec   = static_cast<double>(t.sectorCount) / 75.0;
        printf("  Track %02d  %-5s  LBA %-6u  %5.1f s  (%.0f sectors)\n",
               t.number,
               t.isAudio ? "AUDIO" : "DATA ",
               t.lba,
               lenSec,
               static_cast<double>(t.sectorCount));
        (void)startSec;
    }

    std::string discId = metadata::DiscId::calculate(toc);
    if (!discId.empty())
        printf("  MusicBrainz Disc ID: %s\n", discId.c_str());
}

static void listDrives() {
    auto drives = drive::DriveEnumerator::enumerate();

    if (drives.empty()) {
        printf("No optical drives found.\n");
        return;
    }

    printf("Found %zu optical drive(s):\n\n", drives.size());
    for (const auto& drv : drives) {
        printf("[%s]  %s\n", drv.path().c_str(), drv.description().c_str());

        auto status = drv.status();
        switch (status) {
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

static void printUsage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  (none)          List all optical drives and disc info\n");
    printf("  --list-drives   Same as above\n");
    printf("  --toc <drive>   Read and display TOC for a specific drive (e.g. D:)\n");
    printf("  --help          Show this help\n");
}

int main(int argc, char* argv[]) {
    printf("AtomicRipper v0.1.0 — CD Drive Inspector\n");
    printf("==========================================\n\n");

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "--list-drives") == 0)) {
        listDrives();
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printUsage(argv[0]);
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "--toc") == 0) {
        std::string path = argv[2];
        // Accept either "D" or "D:" or "D:\"
        if (path.size() == 1) path += ':';
        if (path.back() == '\\') path.pop_back();

        drive::Drive drv(path, path);
        if (!drv.isReady()) {
            printf("Drive %s is not ready (no disc or drive error).\n", path.c_str());
            return 1;
        }
        auto toc = drv.readTOC();
        if (!toc) {
            printf("Failed to read TOC from %s.\n", path.c_str());
            return 1;
        }
        printTOC(*toc);
        return 0;
    }

    printUsage(argv[0]);
    return 1;
}
