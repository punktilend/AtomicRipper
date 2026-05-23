#include "CueToolsDB.hpp"

#include <cpr/cpr.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>

namespace atomicripper::metadata {
namespace {

std::string decodeEntities(std::string s) {
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    replaceAll("&quot;", "\"");
    replaceAll("&apos;", "'");
    replaceAll("&lt;", "<");
    replaceAll("&gt;", ">");
    replaceAll("&amp;", "&");
    return s;
}

std::string attr(const std::string& tag, const char* name) {
    const std::regex pattern(std::string("\\s") + name + "=\"([^\"]*)\"",
                             std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(tag, match, pattern))
        return {};
    return decodeEntities(match[1].str());
}

std::string firstElementText(const std::string& block, const char* name) {
    const std::regex pattern(std::string("<") + name + R"(\b[^>]*>([\s\S]*?)</)" + name + ">",
                             std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(block, match, pattern))
        return {};
    return decodeEntities(match[1].str());
}

std::string buildTocString(const drive::TOC& toc) {
    std::ostringstream out;
    bool first = true;
    for (const auto& track : toc.tracks) {
        if (!first) out << ':';
        first = false;
        if (!track.isAudio)
            out << '-';
        out << track.lba;
    }
    if (!first)
        out << ':';
    out << toc.leadOutLBA;
    return out.str();
}

int parsePositiveInt(const std::string& value, int fallback) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        }))
        return fallback;
    return (std::max)(1, std::stoi(value));
}

MbRelease parseMetadataBlock(const std::string& block, const drive::TOC& toc, int index) {
    const size_t tagEnd = block.find('>');
    const std::string rootTag = tagEnd == std::string::npos ? block : block.substr(0, tagEnd + 1);

    MbRelease release;
    release.id = attr(rootTag, "id");
    if (release.id.empty())
        release.id = "ctdb-" + std::to_string(index);
    release.artist = attr(rootTag, "artist");
    release.title = attr(rootTag, "album");
    release.date = attr(rootTag, "year");
    release.genre = attr(rootTag, "genre");
    release.discNumber = parsePositiveInt(attr(rootTag, "discnumber"), 1);
    release.totalDiscs = parsePositiveInt(attr(rootTag, "disccount"), 1);
    release.comment = firstElementText(block, "extra");

    if (release.artist.empty()) release.artist = "Unknown Artist";
    if (release.title.empty()) release.title = "Unknown Title";

    std::vector<MbTrack> tracks;
    const std::regex trackPattern(R"(<track\b[^>]*/>|<track\b[^>]*>[\s\S]*?</track>)",
                                  std::regex_constants::icase);
    int number = 1;
    for (auto it = std::sregex_iterator(block.begin(), block.end(), trackPattern);
         it != std::sregex_iterator(); ++it) {
        const std::string trackTag = it->str();
        MbTrack track;
        track.number = number++;
        track.title = attr(trackTag, "name");
        track.artist = attr(trackTag, "artist");
        if (track.title.empty())
            track.title = "Track" + (track.number < 10 ? std::string("0") : std::string()) +
                          std::to_string(track.number);
        if (track.artist.empty())
            track.artist = release.artist;
        tracks.push_back(std::move(track));
    }

    if (tracks.empty()) {
        for (const auto& tocTrack : toc.tracks) {
            if (!tocTrack.isAudio) continue;
            MbTrack track;
            track.number = tocTrack.number;
            track.title = "Track" + (track.number < 10 ? std::string("0") : std::string()) +
                          std::to_string(track.number);
            track.artist = release.artist;
            track.lengthMs = static_cast<int>(
                (static_cast<uint64_t>(tocTrack.sectorCount) * 1000u) / 75u);
            tracks.push_back(std::move(track));
        }
    }

    release.tracks = std::move(tracks);
    return release;
}

} // namespace

MbResult CueToolsDB::lookup(const drive::TOC& toc) {
    MbResult result;
    if (!toc.isValid()) {
        result.error = "invalid TOC";
        return result;
    }

    const auto response = cpr::Get(
        cpr::Url{"http://db.cuetools.net/lookup2.php"},
        cpr::Parameters{
            {"version", "3"},
            {"ctdb", "0"},
            {"fuzzy", "1"},
            {"metadata", "default"},
            {"toc", buildTocString(toc)}
        },
        cpr::Timeout{15000},
        cpr::Header{{"User-Agent", "AtomicRipper/0.8 CTDB metadata"}});

    if (response.status_code == 404) {
        result.ok = true;
        result.error = "disc not found in CUETools DB";
        return result;
    }
    if (response.status_code != 200 || response.text.empty()) {
        result.error = "CUETools DB HTTP error " + std::to_string(response.status_code);
        return result;
    }

    const std::regex metadataPattern(R"(<metadata\b[^>]*(?:/>|>[\s\S]*?</metadata>))",
                                     std::regex_constants::icase);
    int index = 0;
    for (auto it = std::sregex_iterator(response.text.begin(), response.text.end(), metadataPattern);
         it != std::sregex_iterator(); ++it) {
        result.releases.push_back(parseMetadataBlock(it->str(), toc, index++));
    }

    result.ok = true;
    if (result.releases.empty())
        result.error = "CUETools DB did not return metadata";
    return result;
}

} // namespace atomicripper::metadata
