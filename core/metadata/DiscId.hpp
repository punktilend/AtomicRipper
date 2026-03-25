#pragma once
#include "../drive/TOC.hpp"
#include <string>

namespace atomicripper::metadata {

// Computes the MusicBrainz Disc ID from a TOC.
//
// Algorithm (https://musicbrainz.org/doc/Disc_ID_Calculation):
//   1. Build an 804-character ASCII string:
//      - first track (2 hex digits, uppercase)
//      - last  track (2 hex digits, uppercase)
//      - offset of lead-out in sectors + 150  (8 hex digits, uppercase)
//      - offsets of tracks 1–99 in sectors + 150 (8 hex digits each, padded with zeros)
//   2. SHA-1 hash the string
//   3. Base64-encode with modified alphabet: '+' → '.' , '/' → '_' , '=' → '-'
//
// The resulting string (28 characters) uniquely identifies the disc layout
// and is used to query the MusicBrainz database.
class DiscId {
public:
    // Returns empty string if TOC is invalid or SHA-1 fails
    static std::string calculate(const drive::TOC& toc);

    // Returns the MusicBrainz lookup URL for a given disc ID
    static std::string lookupUrl(const std::string& discId);
};

} // namespace atomicripper::metadata
