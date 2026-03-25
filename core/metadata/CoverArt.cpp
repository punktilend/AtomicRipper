#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "CoverArt.hpp"

#include <cpr/cpr.h>

namespace atomicripper::metadata {

// ---------------------------------------------------------------------------
// detectMimeType — sniff image format from magic bytes
// ---------------------------------------------------------------------------
std::string CoverArt::detectMimeType(const std::vector<uint8_t>& data) {
    if (data.size() >= 3 &&
        data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return "image/jpeg";

    if (data.size() >= 4 &&
        data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47)
        return "image/png";

    return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// fetchFront — HTTP GET the CAA front-cover redirect
// ---------------------------------------------------------------------------
CoverArtResult CoverArt::fetchFront(const std::string& mbReleaseId) {
    CoverArtResult result;

    if (mbReleaseId.empty()) {
        result.error = "No MusicBrainz release ID provided";
        return result;
    }

    const std::string url =
        "https://coverartarchive.org/release/" + mbReleaseId + "/front";

    // cpr follows redirects by default (libcurl behaviour) — no extra option needed
    cpr::Response resp = cpr::Get(
        cpr::Url{url},
        cpr::Header{{"User-Agent", "AtomicRipper/0.6 ( https://github.com/punktilend/AtomicRipper )"}},
        cpr::Timeout{15000}
    );

    if (resp.error) {
        result.error = "HTTP error: " + resp.error.message;
        return result;
    }
    if (resp.status_code == 404) {
        result.error = "No cover art found for release " + mbReleaseId;
        return result;
    }
    if (resp.status_code != 200) {
        result.error = "HTTP " + std::to_string(resp.status_code) + " from Cover Art Archive";
        return result;
    }
    if (resp.text.empty()) {
        result.error = "Empty response from Cover Art Archive";
        return result;
    }

    // Copy binary body into the result vector
    result.data.assign(
        reinterpret_cast<const uint8_t*>(resp.text.data()),
        reinterpret_cast<const uint8_t*>(resp.text.data()) + resp.text.size());

    result.mimeType = detectMimeType(result.data);
    result.ok       = true;
    return result;
}

} // namespace atomicripper::metadata
