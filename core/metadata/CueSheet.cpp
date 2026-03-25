#include "CueSheet.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace atomicripper::metadata {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Strip characters that are illegal in filenames (same set as Pipeline's sanitise)
std::string sanitise(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<'  || c == '>' || c == '|')
            out += '_';
        else
            out += static_cast<char>(c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    return out.empty() ? "_" : out;
}

// Convert an LBA sector number to cue MM:SS:FF format.
// 75 frames per second; LBA counts from track start (always 00:00:00 in per-track mode).
std::string lbaToMsf(uint32_t lba) {
    const uint32_t frames  =  lba % 75;
    const uint32_t seconds = (lba / 75) % 60;
    const uint32_t minutes =  lba / (75 * 60);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", minutes, seconds, frames);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// filename() — derive a .cue filename from MB metadata
// ---------------------------------------------------------------------------
std::string CueSheet::filename(const MbRelease* release) {
    if (!release)
        return "disc.cue";

    std::string name = sanitise(release->artist) + " - " + sanitise(release->title);
    if (name == " - " || name.empty())
        return "disc.cue";
    return name + ".cue";
}

// ---------------------------------------------------------------------------
// generate() — build cue sheet content
// ---------------------------------------------------------------------------
std::string CueSheet::generate(
    const drive::TOC&                         toc,
    const std::vector<std::filesystem::path>& filePaths,
    const MbRelease*                          release,
    const std::string&                        discId,
    const std::vector<uint64_t>&              trackSampleOffsets)
{
    std::ostringstream out;

    // -- Global REM / metadata fields --------------------------------------
    if (!discId.empty())
        out << "REM DISCID " << discId << "\r\n";

    if (release && !release->date.empty()) {
        // date may be "1973-03-01" — take just the year
        out << "REM DATE " << release->date.substr(0, 4) << "\r\n";
    }

    out << "REM COMMENT \"AtomicRipper\"\r\n";

    const std::string albumArtist = release ? release->artist    : "";
    const std::string albumTitle  = release ? release->title     : "";
    const std::string albumLabel  = release ? release->label     : "";

    if (!albumLabel.empty())
        out << "REM LABEL \"" << albumLabel << "\"\r\n";

    if (!albumArtist.empty())
        out << "PERFORMER \"" << albumArtist << "\"\r\n";

    if (!albumTitle.empty())
        out << "TITLE \"" << albumTitle << "\"\r\n";

    // -- Per-track entries -------------------------------------------------
    const bool singleFile = !trackSampleOffsets.empty();

    // Single-file mode: one FILE line before the first track
    if (singleFile) {
        const std::string fileEntry =
            (!filePaths.empty() && !filePaths[0].empty())
                ? filePaths[0].filename().string()
                : "disc.flac";
        out << "FILE \"" << fileEntry << "\" WAVE\r\n";
    }

    int audioIdx = 0;
    for (const auto& track : toc.tracks) {
        if (!track.isAudio) continue;

        if (!singleFile) {
            // Per-track mode: each track gets its own FILE line
            const bool hasFile = (audioIdx < static_cast<int>(filePaths.size()) &&
                                  !filePaths[static_cast<size_t>(audioIdx)].empty());
            const std::string fileEntry = hasFile
                ? filePaths[static_cast<size_t>(audioIdx)].filename().string()
                : ("track" + std::to_string(track.number) + ".flac");
            out << "FILE \"" << fileEntry << "\" WAVE\r\n";
        }

        // TRACK block
        char trackNum[8];
        std::snprintf(trackNum, sizeof(trackNum), "%02d", track.number);
        out << "  TRACK " << trackNum << " AUDIO\r\n";

        // Track-level metadata from MB
        if (release && audioIdx < static_cast<int>(release->tracks.size())) {
            const auto& mbTrack = release->tracks[static_cast<size_t>(audioIdx)];
            if (!mbTrack.title.empty())
                out << "    TITLE \"" << mbTrack.title << "\"\r\n";

            const std::string trackArtist =
                mbTrack.artist.empty() ? albumArtist : mbTrack.artist;
            if (!trackArtist.empty())
                out << "    PERFORMER \"" << trackArtist << "\"\r\n";
        }

        // INDEX 01: per-track = 00:00:00; single-file = cumulative MSF offset
        uint32_t lba = 0;
        if (singleFile && audioIdx < static_cast<int>(trackSampleOffsets.size())) {
            // 588 samples per CD sector (44100 Hz / 75 frames per second)
            lba = static_cast<uint32_t>(
                trackSampleOffsets[static_cast<size_t>(audioIdx)] / 588u);
        }
        out << "    INDEX 01 " << lbaToMsf(lba) << "\r\n";

        ++audioIdx;
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// write()
// ---------------------------------------------------------------------------
bool CueSheet::write(const std::filesystem::path& outputPath,
                      const std::string&           content,
                      std::string*                 error) {
    std::ofstream f(outputPath, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot create cue sheet: " + outputPath.string();
        return false;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) {
        if (error) *error = "write failed: " + outputPath.string();
        return false;
    }
    return true;
}

} // namespace atomicripper::metadata
