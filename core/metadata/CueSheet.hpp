#pragma once
#include "../drive/TOC.hpp"
#include "MusicBrainz.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace atomicripper::metadata {

// ---------------------------------------------------------------------------
// CueSheet — generates a CD cue sheet (.cue) from TOC + optional MB metadata
//
// Per-track mode (one FLAC/WAV file per track):
//   Each track gets its own FILE entry with INDEX 01 00:00:00, since every
//   output file starts at the beginning of that track's audio.
//
// Format reference: Cue Sheet Specification
//   https://wiki.hydrogenaud.io/index.php?title=Cue_sheet
// ---------------------------------------------------------------------------
class CueSheet {
public:
    // Build the cue sheet content as a UTF-8 string.
    //
    // Per-track mode (trackSampleOffsets empty):
    //   filePaths has one path per audio track; each gets its own FILE line with
    //   INDEX 01 00:00:00.
    //
    // Single-file mode (trackSampleOffsets non-empty):
    //   filePaths[0] is the single output file; trackSampleOffsets[i] is the PCM
    //   sample offset of audio track i from the start of that file.  Offsets are
    //   converted to MM:SS:FF (75 frames/sec) for the INDEX 01 lines.
    //
    // release:  nullptr if MusicBrainz metadata is not available.
    // discId:   MusicBrainz disc ID (written as REM DISCID).
    static std::string generate(
        const drive::TOC&                          toc,
        const std::vector<std::filesystem::path>&  filePaths,
        const MbRelease*                           release,
        const std::string&                         discId = {},
        const std::vector<uint64_t>&               trackSampleOffsets = {});

    // Write content to outputPath.  Returns false and sets error on failure.
    static bool write(
        const std::filesystem::path& outputPath,
        const std::string&           content,
        std::string*                 error = nullptr);

    // Derive a sensible filename for the cue sheet from release metadata.
    // Falls back to "disc.cue" when release is nullptr.
    static std::string filename(const MbRelease* release);
};

} // namespace atomicripper::metadata
