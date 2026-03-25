#include <core/drive/DriveEnumerator.hpp>
#include <core/drive/TOC.hpp>
#include <core/encode/FlacEncoder.hpp>
#include <core/encode/IEncoder.hpp>
#include <core/metadata/DiscId.hpp>
#include <core/metadata/MusicBrainz.hpp>
#include <core/metadata/TagWriter.hpp>
#include <core/rip/RipEngine.hpp>
#include <core/verify/AccurateRip.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace atomicripper;

// ---------------------------------------------------------------------------
// WAV fallback writer
// ---------------------------------------------------------------------------
namespace {
void writeLE16(FILE* fp, uint16_t v) {
    uint8_t b[2] = { uint8_t(v & 0xFF), uint8_t(v >> 8) };
    fwrite(b, 1, 2, fp);
}
void writeLE32(FILE* fp, uint32_t v) {
    uint8_t b[4] = { uint8_t(v & 0xFF), uint8_t((v>>8)&0xFF),
                     uint8_t((v>>16)&0xFF), uint8_t((v>>24)&0xFF) };
    fwrite(b, 1, 4, fp);
}
void writeWavHeader(FILE* fp, uint32_t dataBytes) {
    fwrite("RIFF",1,4,fp); writeLE32(fp, dataBytes+36);
    fwrite("WAVE",1,4,fp); fwrite("fmt ",1,4,fp); writeLE32(fp,16);
    writeLE16(fp,1); writeLE16(fp,2); writeLE32(fp,44100); writeLE32(fp,176400);
    writeLE16(fp,4); writeLE16(fp,16); fwrite("data",1,4,fp); writeLE32(fp,dataBytes);
}
} // namespace

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
// Sanitise a string for use in a filename (replace illegal chars with '_')
// ---------------------------------------------------------------------------
static std::string sanitise(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<'  || c == '>' || c == '|')
            out += '_';
        else
            out += static_cast<char>(c);
    }
    // Trim trailing dots/spaces (Windows forbidden)
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    return out.empty() ? "_" : out;
}

// ---------------------------------------------------------------------------
// Rip command
// ---------------------------------------------------------------------------
static int doRip(const std::string& drivePath,
                 const std::string& outputDir,
                 encode::Format     format,
                 int                driveOffset) {
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
        printf("Drive %s is not ready.\n", drivePath.c_str()); return 1;
    }
    auto toc = drv.readTOC();
    if (!toc || !toc->isValid()) {
        printf("Failed to read TOC from %s.\n", drivePath.c_str()); return 1;
    }

    const char* fmtName = (format == encode::Format::WAV) ? "WAV" : "FLAC";
    printf("Drive  : %s\n", drivePath.c_str());
    printf("Disc   : %d tracks, %.0f sec\n", toc->audioTrackCount(), toc->durationSeconds());
    printf("Format : %s\n", fmtName);
    if (driveOffset != 0)
        printf("Offset : %+d samples\n", driveOffset);
    printf("Output : %s\n\n", outPath.string().c_str());

    // Open rip engine
    rip::RipSettings ripSettings;
    ripSettings.mode        = rip::RipMode::Secure;
    ripSettings.maxRetries  = 16;
    ripSettings.minMatches  = 2;
    ripSettings.useC2Errors = true;
    ripSettings.driveOffset = driveOffset;

    rip::RipEngine engine(drivePath, ripSettings);
    if (!engine.open()) {
        printf("Error: could not open drive %s for SCSI access.\n", drivePath.c_str());
        printf("  (Try running as Administrator)\n");
        return 1;
    }
    engine.probeCapabilities();
    printf("C2 support: %s\n\n", engine.supportsC2() ? "yes" : "no");

    const int totalAudioTracks = toc->audioTrackCount();
    int       tracksWritten    = 0;
    int       totalErrors      = 0;

    std::vector<std::vector<uint8_t>> allTrackPcm;
    std::vector<std::filesystem::path> writtenFiles;   // for tagging later
    allTrackPcm.reserve(static_cast<size_t>(totalAudioTracks));
    writtenFiles.reserve(static_cast<size_t>(totalAudioTracks));

    // -----------------------------------------------------------------------
    // Rip loop
    // -----------------------------------------------------------------------
    for (const auto& track : toc->tracks) {
        if (!track.isAudio) {
            printf("Track %02d: DATA — skipping\n\n", track.number);
            continue;
        }

        printf("Ripping track %02d/%d  (%u sectors, %.1f sec)\n",
               track.number, totalAudioTracks,
               track.sectorCount, static_cast<double>(track.sectorCount)/75.0);

        auto onProgress = [](const rip::RipProgress& p) {
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

        auto result = engine.ripTrack(track, track.number, onProgress);
        printf("\n");

        if (result.cancelled) { printf("  Cancelled.\n"); return 1; }
        if (!result.ok) {
            printf("  ERROR: rip failed for track %02d\n\n", track.number);
            ++totalErrors;
            allTrackPcm.push_back({});
            writtenFiles.push_back({});
            continue;
        }

        int suspectSectors = 0, c2Sectors = 0;
        for (const auto& sr : result.sectors) {
            if (sr.confidence == 0) ++suspectSectors;
            if (sr.hasC2Errors)     ++c2Sectors;
        }
        printf("  CRC32: %08X  |  rip: %s  |  C2: %d sector(s)\n",
               result.crc32,
               suspectSectors == 0 ? "OK" : "SUSPECT",
               c2Sectors);
        if (suspectSectors > 0)
            printf("  WARNING: %d sector(s) could not reach read consensus\n", suspectSectors);

        // Write file
        char filename[64];
        bool writeOk = false;
        std::filesystem::path filePath;

        if (format == encode::Format::FLAC) {
            snprintf(filename, sizeof(filename), "track%02d.flac", track.number);
            filePath = outPath / filename;

            encode::EncoderSettings encSettings;
            encSettings.format           = encode::Format::FLAC;
            encSettings.compressionLevel = 8;
            encSettings.totalSamples     = static_cast<uint64_t>(result.data.size()) / 4u;

            encode::FlacEncoder encoder;
            encoder.setTag("TRACKNUMBER", std::to_string(track.number));
            encoder.setTag("TRACKTOTAL",  std::to_string(totalAudioTracks));

            if (!encoder.open(filePath, encSettings)) {
                printf("  ERROR: %s\n", encoder.lastError().c_str());
                ++totalErrors; allTrackPcm.push_back({}); writtenFiles.push_back({}); continue;
            }
            std::vector<int32_t> samples;
            encode::cdBytesToSamples(result.data.data(), result.data.size(), samples);
            if (!encoder.writeSamples(samples) || !encoder.finalize()) {
                printf("  ERROR: encode failed: %s\n", encoder.lastError().c_str());
                ++totalErrors; allTrackPcm.push_back({}); writtenFiles.push_back({}); continue;
            }
            writeOk = true;

        } else {
            snprintf(filename, sizeof(filename), "track%02d.wav", track.number);
            filePath = outPath / filename;
            FILE* fp = fopen(filePath.string().c_str(), "wb");
            if (!fp) {
                printf("  ERROR: cannot write %s\n", filePath.string().c_str());
                ++totalErrors; allTrackPcm.push_back({}); writtenFiles.push_back({}); continue;
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
            printf("  -> %s\n\n", filePath.string().c_str());
            ++tracksWritten;
        }

        allTrackPcm.push_back(std::move(result.data));
        writtenFiles.push_back(writeOk ? filePath : std::filesystem::path{});
    }

    if (tracksWritten == 0) {
        printf("No tracks written.\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // AccurateRip verification
    // -----------------------------------------------------------------------
    printf("AccurateRip verification...\n");
    if (static_cast<int>(allTrackPcm.size()) == totalAudioTracks) {
        auto arResult = verify::AccurateRip::verify(*toc, allTrackPcm);

        if (!arResult.lookupOk) {
            printf("  Lookup failed: %s\n\n", arResult.error.c_str());
        } else if (!arResult.error.empty()) {
            printf("  %s\n\n", arResult.error.c_str());
        } else {
            printf("  DB pressings found: %d\n", arResult.dbEntries);
            int matched = 0;
            for (const auto& tr : arResult.tracks) {
                int conf = tr.confidenceV2 > 0 ? tr.confidenceV2 : tr.confidenceV1;
                const char* ver = tr.confidenceV2 > 0 ? "v2" : (tr.confidenceV1 > 0 ? "v1" : "--");
                printf("  Track %02d: %s  conf=%-3d  CRCv1=%08X  CRCv2=%08X\n",
                       tr.trackNumber,
                       tr.matched ? "OK      " : "NO MATCH",
                       conf, tr.checksumV1, tr.checksumV2);
                if (tr.matched) ++matched;
            }
            printf("  %d/%d tracks verified accurately\n", matched, totalAudioTracks);
            if (matched < totalAudioTracks && driveOffset == 0)
                printf("  Hint: if no tracks match, try --offset to set your drive's read offset.\n");
        }
    }
    printf("\n");

    // -----------------------------------------------------------------------
    // MusicBrainz metadata lookup + tagging (FLAC only)
    // -----------------------------------------------------------------------
    if (format == encode::Format::FLAC) {
        std::string discId = metadata::DiscId::calculate(*toc);
        printf("MusicBrainz lookup  (disc ID: %s)...\n", discId.c_str());

        auto mbResult = metadata::MusicBrainz::lookup(discId);

        if (!mbResult.ok || mbResult.releases.empty()) {
            if (!mbResult.error.empty())
                printf("  %s\n", mbResult.error.c_str());
            else
                printf("  No releases found.\n");
            printf("  Skipping tag write — tracks already have TRACKNUMBER tags.\n\n");

        } else {
            // Show available releases
            printf("  Found %zu release(s):\n\n", mbResult.releases.size());
            for (size_t i = 0; i < mbResult.releases.size(); ++i) {
                const auto& r = mbResult.releases[i];
                printf("  [%zu] %s — %s (%s)%s\n",
                       i + 1,
                       r.title.c_str(),
                       r.artist.c_str(),
                       r.date.empty() ? "?" : r.date.substr(0, 4).c_str(),
                       r.country.empty() ? "" : (" [" + r.country + "]").c_str());
            }

            // Pick a release
            int selectedIdx = 0;
            if (mbResult.releases.size() > 1) {
                printf("\n  Select [1-%zu] (Enter = #1): ", mbResult.releases.size());
                fflush(stdout);
                char buf[16] = {};
                if (fgets(buf, sizeof(buf), stdin)) {
                    int choice = std::atoi(buf);
                    if (choice >= 1 && choice <= static_cast<int>(mbResult.releases.size()))
                        selectedIdx = choice - 1;
                }
            }

            const auto& release = mbResult.releases[static_cast<size_t>(selectedIdx)];
            printf("\n  Using: %s — %s\n\n", release.title.c_str(), release.artist.c_str());

            // Write tags + rename files to artist/album/track pattern
            int tagged = 0;
            int audioIdx = 0;  // index into writtenFiles / allTrackPcm

            for (const auto& track : toc->tracks) {
                if (!track.isAudio) continue;

                const auto& filePath = writtenFiles[static_cast<size_t>(audioIdx)];
                ++audioIdx;

                if (filePath.empty()) continue;  // this track failed to rip

                // Match track number to MB track list (0-based index)
                int mbIdx = track.number - 1;
                if (mbIdx < 0 || mbIdx >= static_cast<int>(release.tracks.size()))
                    mbIdx = audioIdx - 1;  // fallback: positional

                auto tags = metadata::TrackTags::from(release, mbIdx);

                std::string tagError;
                if (!metadata::TagWriter::writeFlac(filePath, tags, &tagError)) {
                    printf("  Track %02d tag error: %s\n", track.number, tagError.c_str());
                    continue;
                }

                // Rename to descriptive filename: NN - Title.flac
                const std::string newName =
                    std::to_string(track.number / 10) +
                    std::to_string(track.number % 10) + " - " +
                    sanitise(tags.title) + ".flac";

                std::filesystem::path newPath = filePath.parent_path() / newName;
                std::error_code renameEc;
                std::filesystem::rename(filePath, newPath, renameEc);
                if (!renameEc)
                    printf("  Track %02d: %s\n", track.number, newName.c_str());
                else
                    printf("  Track %02d: tagged (rename failed: %s)\n",
                           track.number, renameEc.message().c_str());

                ++tagged;
            }
            printf("\n  %d/%d tracks tagged.\n", tagged, tracksWritten);
        }
        printf("\n");
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
    printf("  (none)                              List all optical drives\n");
    printf("  --list-drives                       Same as above\n");
    printf("  --toc <drive>                       Read and display TOC (e.g. D:)\n");
    printf("  --rip <drive> [outdir]              Rip all audio tracks to FLAC\n");
    printf("  --rip <drive> [outdir] --wav        Rip to WAV instead\n");
    printf("  --rip <drive> [outdir] --offset N   Set drive read offset in samples\n");
    printf("  --help                              Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --toc D:\n", prog);
    printf("  %s --rip D:\n", prog);
    printf("  %s --rip D: \"C:\\Music\\MyDisc\"\n", prog);
    printf("  %s --rip D: --offset 30\n", prog);
}

int main(int argc, char* argv[]) {
    printf("AtomicRipper v0.5.0\n");
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
        encode::Format fmt    = encode::Format::FLAC;
        int            offset = 0;

        for (int i = 3; i < argc; ++i) {
            if      (strcmp(argv[i], "--wav") == 0)
                fmt = encode::Format::WAV;
            else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc)
                offset = std::atoi(argv[++i]);
            else if (outDir.empty())
                outDir = argv[i];
        }
        return doRip(path, outDir, fmt, offset);
    }

    printUsage(argv[0]); return 1;
}
