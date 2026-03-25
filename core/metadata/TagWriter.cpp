#include "TagWriter.hpp"

#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>

namespace atomicripper::metadata {

// ---------------------------------------------------------------------------
// TrackTags::from — convenience builder from a MbRelease + track index
// ---------------------------------------------------------------------------
TrackTags TrackTags::from(const MbRelease& release, int trackIndex) {
    TrackTags t;

    t.album           = release.title;
    t.albumArtist     = release.artist;
    t.albumArtistSort = release.artistSortName;
    t.date            = release.date;
    t.label           = release.label;
    t.mbReleaseId     = release.id;
    t.mbAlbumArtistId = release.artistId;
    t.discNumber      = std::to_string(release.discNumber);
    t.discTotal       = std::to_string(release.totalDiscs);
    t.trackTotal      = std::to_string(static_cast<int>(release.tracks.size()));

    if (trackIndex >= 0 && trackIndex < static_cast<int>(release.tracks.size())) {
        const auto& tr = release.tracks[trackIndex];
        t.title         = tr.title;
        t.artist        = tr.artist.empty() ? release.artist : tr.artist;
        t.trackNumber   = std::to_string(tr.number);
        t.mbRecordingId = tr.recordingId;
        t.mbArtistId    = tr.artistId.empty() ? release.artistId : tr.artistId;
    }

    return t;
}

// ---------------------------------------------------------------------------
// TagWriter::writeFlac
// ---------------------------------------------------------------------------
bool TagWriter::writeFlac(const std::filesystem::path& filePath,
                           const TrackTags&             tags,
                           std::string*                 error) {
    auto setErr = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };

    // Open file — use wide path on Windows for Unicode safety
#ifdef _WIN32
    TagLib::FLAC::File file(filePath.wstring().c_str());
#else
    TagLib::FLAC::File file(filePath.c_str());
#endif

    if (!file.isValid())
        return setErr("TagLib: could not open FLAC file");

    // Get (or create) the XiphComment block — this is the native tag for FLAC
    TagLib::Ogg::XiphComment* xc = file.xiphComment(/*create=*/true);
    if (!xc)
        return setErr("TagLib: could not access XiphComment block");

    // Helper: add a field only if value is non-empty; always remove old value first
    auto set = [&](const char* field, const std::string& value) {
        xc->removeFields(field);
        if (!value.empty())
            xc->addField(field, TagLib::String(value, TagLib::String::UTF8));
    };

    set("TITLE",                tags.title);
    set("ARTIST",               tags.artist);
    set("ALBUMARTIST",          tags.albumArtist);
    set("ALBUMARTISTSORT",      tags.albumArtistSort);
    set("ALBUM",                tags.album);
    set("DATE",                 tags.date);
    set("TRACKNUMBER",          tags.trackNumber);
    set("TRACKTOTAL",           tags.trackTotal);
    set("TOTALTRACKS",          tags.trackTotal);   // legacy alias
    set("DISCNUMBER",           tags.discNumber);
    set("DISCTOTAL",            tags.discTotal);
    set("TOTALDISCS",           tags.discTotal);    // legacy alias
    set("LABEL",                tags.label);
    set("COMMENT",              tags.comment);

    // MusicBrainz IDs
    set("MUSICBRAINZ_TRACKID",        tags.mbRecordingId);
    set("MUSICBRAINZ_ALBUMID",        tags.mbReleaseId);
    set("MUSICBRAINZ_ARTISTID",       tags.mbArtistId);
    set("MUSICBRAINZ_ALBUMARTISTID",  tags.mbAlbumArtistId);

    if (!file.save())
        return setErr("TagLib: save failed");

    return true;
}

} // namespace atomicripper::metadata
