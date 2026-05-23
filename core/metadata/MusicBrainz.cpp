#include "MusicBrainz.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#endif

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <thread>

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

std::string firstGenreName(const json& rel) {
    auto bestFrom = [](const json& entries) {
        std::string best;
        int bestCount = -1;
        if (!entries.is_array())
            return best;
        for (const auto& entry : entries) {
            const std::string name = entry.value("name", "");
            if (name.empty())
                continue;
            const int count = entry.value("count", 0);
            if (best.empty() || count > bestCount) {
                best = name;
                bestCount = count;
            }
        }
        return best;
    };

    if (rel.contains("genres")) {
        const std::string genre = bestFrom(rel["genres"]);
        if (!genre.empty())
            return genre;
    }
    if (rel.contains("tags"))
        return bestFrom(rel["tags"]);
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

    r.genre = firstGenreName(rel);

    // Label info (first label in list, if present)
    if (rel.contains("label-info") && !rel["label-info"].empty()) {
        const auto& li = rel["label-info"][0];
        if (li.contains("label"))
            r.label = li["label"].value("name", "");
        r.catalogNumber = li.value("catalog-number", "");
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

void mergeReleaseDetails(MbRelease& base, MbRelease detail) {
    if (base.id.empty())             base.id = std::move(detail.id);
    if (base.title.empty())          base.title = std::move(detail.title);
    if (base.artist.empty())         base.artist = std::move(detail.artist);
    if (base.artistId.empty())       base.artistId = std::move(detail.artistId);
    if (base.artistSortName.empty()) base.artistSortName = std::move(detail.artistSortName);
    if (base.date.empty())           base.date = std::move(detail.date);
    if (base.country.empty())        base.country = std::move(detail.country);
    if (base.label.empty())          base.label = std::move(detail.label);
    if (base.catalogNumber.empty())  base.catalogNumber = std::move(detail.catalogNumber);
    if (base.genre.empty())          base.genre = std::move(detail.genre);
    if (base.tracks.empty())         base.tracks = std::move(detail.tracks);
    if (base.discNumber <= 1)        base.discNumber = detail.discNumber;
    if (base.totalDiscs <= 1)        base.totalDiscs = detail.totalDiscs;
}

std::string tocQuery(const drive::TOC& toc) {
    std::vector<const drive::TrackInfo*> audioTracks;
    audioTracks.reserve(toc.tracks.size());
    for (const auto& track : toc.tracks) {
        if (track.isAudio)
            audioTracks.push_back(&track);
    }
    if (audioTracks.empty())
        return {};

    std::ostringstream out;
    out << audioTracks.front()->number << '+'
        << audioTracks.back()->number << '+'
        << (toc.leadOutLBA + 150);

    for (const auto* track : audioTracks)
        out << '+' << (track->lba + 150);

    return out.str();
}

} // namespace

// ---------------------------------------------------------------------------
// MusicBrainz::lookup
// ---------------------------------------------------------------------------
MbResult MusicBrainz::lookup(const std::string& discId) {
    drive::TOC emptyToc;
    return lookup(discId, emptyToc);
}

MbResult MusicBrainz::lookup(const std::string& discId, const drive::TOC& toc) {
    MbResult result;
    result.discId = discId;

    if (discId.empty()) {
        result.error = "empty disc ID";
        return result;
    }

    // MB Web Service v2 — disc ID lookup with recordings and artists.
    // "label-info" is valid on release lookups, but not on the discid resource.
    const std::string tocParam = toc.isValid() ? tocQuery(toc) : std::string{};
    const std::string url =
        "https://musicbrainz.org/ws/2/discid/" + discId;

    cpr::Parameters params{
        {"fmt", "json"},
        {"inc", "recordings+artists"}
    };
    if (!tocParam.empty())
        params.Add({"toc", tocParam});

    auto response = cpr::Get(
        cpr::Url{url},
        params,
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
        if (!response.text.empty()) {
            try {
                const auto err = json::parse(response.text);
                const std::string msg = err.value("error", "");
                if (!msg.empty())
                    result.error += ": " + msg;
            } catch (...) {
                result.error += ": " + response.text.substr(0, 160);
            }
        }
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

    int enrichedCount = 0;
    for (const auto& rel : j["releases"]) {
        try {
            auto release = parseRelease(rel);
            if (!release.id.empty() && enrichedCount < 5) {
                std::string ignored;
                std::this_thread::sleep_for(std::chrono::milliseconds(1100));
                enrichRelease(release, &ignored);
                ++enrichedCount;
            }
            result.releases.push_back(std::move(release));
        } catch (...) {
            // Skip malformed entries; keep going
        }
    }

    result.ok = true;
    return result;
}

bool MusicBrainz::enrichRelease(MbRelease& release, std::string* error) {
    if (release.id.empty()) {
        if (error) *error = "empty release ID";
        return false;
    }

    const std::string url = "https://musicbrainz.org/ws/2/release/" + release.id;
    auto response = cpr::Get(
        cpr::Url{url},
        cpr::Parameters{
            {"fmt", "json"},
            {"inc", "artists+recordings+labels+genres+tags+media"}
        },
        cpr::Timeout{20000},
        cpr::Header{{
            "User-Agent",
            "AtomicRipper/0.8.0 (https://github.com/punktilend/AtomicRipper)"
        }}
    );

    if (response.status_code != 200) {
        if (error)
            *error = "release detail HTTP " + std::to_string(response.status_code);
        return false;
    }
    if (response.text.empty()) {
        if (error) *error = "empty release detail response";
        return false;
    }

    try {
        auto detail = parseRelease(json::parse(response.text));
        mergeReleaseDetails(release, std::move(detail));
    } catch (const std::exception& e) {
        if (error) *error = std::string("release detail parse error: ") + e.what();
        return false;
    } catch (...) {
        if (error) *error = "release detail parse error";
        return false;
    }

    return true;
}

} // namespace atomicripper::metadata
