#include "MusicBrainz.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#endif

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <sstream>

using json = nlohmann::json;

namespace atomicripper::metadata {

namespace {

// Join an artist-credit array into a display string, respecting joinphrase.
std::string parseArtistCredit(const json& credit) {
    std::string result;
    for (const auto& entry : credit) {
        if (entry.contains("artist"))
            result += entry["artist"].value("name", "");
        result += entry.value("joinphrase", "");
    }
    return result;
}

std::string primaryArtistId(const json& credit) {
    if (!credit.empty() && credit[0].contains("artist"))
        return credit[0]["artist"].value("id", "");
    return {};
}

std::string primaryArtistSort(const json& credit) {
    if (!credit.empty() && credit[0].contains("artist"))
        return credit[0]["artist"].value("sort-name", "");
    return {};
}

MbRelease parseRelease(const json& rel) {
    MbRelease r;
    r.id      = rel.value("id",      "");
    r.title   = rel.value("title",   "");
    r.date    = rel.value("date",    "");
    r.country = rel.value("country", "");

    if (rel.contains("artist-credit")) {
        const auto& ac = rel["artist-credit"];
        r.artist         = parseArtistCredit(ac);
        r.artistId       = primaryArtistId(ac);
        r.artistSortName = primaryArtistSort(ac);
    }

    // Label info (first label in list, if present)
    if (rel.contains("label-info") && !rel["label-info"].empty()) {
        const auto& li = rel["label-info"][0];
        if (li.contains("label"))
            r.label = li["label"].value("name", "");
    }

    // Media — find the disc that has our track count
    if (rel.contains("media")) {
        const auto& media = rel["media"];
        r.totalDiscs = static_cast<int>(media.size());

        for (const auto& medium : media) {
            r.discNumber = medium.value("position", 1);

            if (!medium.contains("tracks")) continue;

            for (const auto& t : medium["tracks"]) {
                MbTrack track;
                track.number   = t.value("position", 0);
                track.title    = t.value("title",    "");
                track.lengthMs = t.value("length",   0);

                // Track artist (may differ from release artist)
                if (t.contains("artist-credit")) {
                    track.artist   = parseArtistCredit(t["artist-credit"]);
                    track.artistId = primaryArtistId(t["artist-credit"]);
                } else {
                    track.artist   = r.artist;
                    track.artistId = r.artistId;
                }

                if (t.contains("recording"))
                    track.recordingId = t["recording"].value("id", "");

                r.tracks.push_back(std::move(track));
            }

            // We only care about the first medium that has tracks
            // (multi-disc handling: the disc ID already identifies the right disc)
            if (!r.tracks.empty()) break;
        }
    }

    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// MusicBrainz::lookup
// ---------------------------------------------------------------------------
MbResult MusicBrainz::lookup(const std::string& discId) {
    MbResult result;
    result.discId = discId;

    if (discId.empty()) {
        result.error = "empty disc ID";
        return result;
    }

    // MB Web Service v2 — disc ID lookup with recordings and artists
    const std::string url =
        "https://musicbrainz.org/ws/2/discid/" + discId +
        "?fmt=json&inc=recordings+artists+label-info";

    auto response = cpr::Get(
        cpr::Url{url},
        cpr::Timeout{20000},
        cpr::Header{{
            "User-Agent",
            "AtomicRipper/0.5.0 (https://github.com/punktilend/AtomicRipper)"
        }}
    );

    if (response.status_code == 404) {
        result.error = "disc not found in MusicBrainz";
        result.ok    = true;  // not a network error — just not in DB
        return result;
    }
    if (response.status_code != 200) {
        result.error = "HTTP " + std::to_string(response.status_code);
        return result;
    }
    if (response.text.empty()) {
        result.error = "empty response";
        return result;
    }

    // Parse JSON
    json j;
    try {
        j = json::parse(response.text);
    } catch (const json::exception& e) {
        result.error = std::string("JSON parse error: ") + e.what();
        return result;
    }

    if (!j.contains("releases") || !j["releases"].is_array()) {
        result.error = "no releases in response";
        result.ok    = true;
        return result;
    }

    for (const auto& rel : j["releases"]) {
        try {
            result.releases.push_back(parseRelease(rel));
        } catch (...) {
            // Skip malformed entries; keep going
        }
    }

    result.ok = true;
    return result;
}

} // namespace atomicripper::metadata
