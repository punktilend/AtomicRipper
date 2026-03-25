#pragma once
#include "MusicBrainz.hpp"
#include <filesystem>
#include <string>

namespace atomicripper::metadata {

// ---------------------------------------------------------------------------
// All tags for a single track — filled from MbRelease + MbTrack
// ---------------------------------------------------------------------------
struct TrackTags {
    std::string title;
    std::string artist;          // track artist
    std::string albumArtist;     // album-level artist (ALBUMARTIST)
    std::string albumArtistSort; // sort form, e.g. "Floyd, Pink"
    std::string album;
    std::string date;            // e.g. "1973" or "1973-03-01"
    std::string trackNumber;     // "1"
    std::string trackTotal;      // "12"
    std::string discNumber;      // "1"
    std::string discTotal;       // "1"
    std::string label;
    std::string comment;

    // MusicBrainz identifiers (written as standard Vorbis Comment fields)
    std::string mbRecordingId;
    std::string mbReleaseId;
    std::string mbArtistId;
    std::string mbAlbumArtistId;

    // Build a TrackTags from a MbRelease + zero-based track index
    static TrackTags from(const MbRelease& release, int trackIndex);
};

// ---------------------------------------------------------------------------
// TagWriter — updates Vorbis Comment tags in an existing FLAC file.
// Replaces all existing tags with the supplied set.
// ---------------------------------------------------------------------------
class TagWriter {
public:
    // Write tags to a FLAC file. Returns false and sets error on failure.
    static bool writeFlac(const std::filesystem::path& filePath,
                          const TrackTags&             tags,
                          std::string*                 error = nullptr);
};

} // namespace atomicripper::metadata
