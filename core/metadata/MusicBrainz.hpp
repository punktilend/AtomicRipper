#pragma once
#include <string>
#include <vector>

namespace atomicripper::metadata {

// ---------------------------------------------------------------------------
// A single track as returned by the MusicBrainz Web Service
// ---------------------------------------------------------------------------
struct MbTrack {
    int         number      = 0;
    std::string title;
    std::string artist;       // track artist (may differ from album artist)
    std::string artistId;     // MusicBrainz artist MBID
    std::string recordingId;  // MusicBrainz recording MBID
    int         lengthMs    = 0;
};

// ---------------------------------------------------------------------------
// A release (album/single/EP) from MusicBrainz
// ---------------------------------------------------------------------------
struct MbRelease {
    std::string id;             // MusicBrainz release MBID
    std::string title;          // album title
    std::string artist;         // album artist name(s), joined
    std::string artistId;       // primary artist MBID
    std::string artistSortName; // e.g. "Floyd, Pink"
    std::string date;           // "1973", "1973-03-01", etc.
    std::string country;        // two-letter ISO country code
    std::string label;          // record label (if available)
    int         discNumber  = 1;
    int         totalDiscs  = 1;

    std::vector<MbTrack> tracks;
};

// ---------------------------------------------------------------------------
// Result of a MusicBrainz disc-ID lookup
// ---------------------------------------------------------------------------
struct MbResult {
    std::vector<MbRelease> releases;
    std::string            discId;   // the 28-char MB disc ID used for lookup
    std::string            error;    // non-empty on failure
    bool                   ok = false;
};

// ---------------------------------------------------------------------------
// MusicBrainz — disc ID lookup via the MB Web Service v2
//
// Endpoint:
//   https://musicbrainz.org/ws/2/discid/{discId}?fmt=json&inc=recordings+artists
//
// Rate limit: 1 request/second (we make only one per rip session, so fine).
// ---------------------------------------------------------------------------
class MusicBrainz {
public:
    // Look up a disc by its MusicBrainz disc ID string.
    // discId must be the 28-character base64url string from DiscId::calculate().
    static MbResult lookup(const std::string& discId);
};

} // namespace atomicripper::metadata
