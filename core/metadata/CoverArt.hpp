#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace atomicripper::metadata {

// ---------------------------------------------------------------------------
// Result of a cover art download attempt
// ---------------------------------------------------------------------------
struct CoverArtResult {
    std::vector<uint8_t> data;       // raw image bytes (JPEG or PNG)
    std::string          mimeType;   // "image/jpeg" or "image/png"
    bool                 ok    = false;
    std::string          error;
};

// ---------------------------------------------------------------------------
// CoverArt — downloads front cover art from the MusicBrainz Cover Art Archive
//
// URL pattern: https://coverartarchive.org/release/{mbReleaseId}/front
// The server redirects to the actual image; cpr follows redirects automatically.
// ---------------------------------------------------------------------------
class CoverArt {
public:
    // Download the front cover for the given MusicBrainz release ID.
    // Returns an empty result (ok=false) if the release has no cover art or
    // if the network request fails.
    static CoverArtResult fetchFront(const std::string& mbReleaseId);

    // Detect MIME type from the first bytes of image data.
    // Returns "image/jpeg", "image/png", or "application/octet-stream".
    static std::string detectMimeType(const std::vector<uint8_t>& data);
};

} // namespace atomicripper::metadata
