#pragma once
#include "../drive/TOC.hpp"
#include "MusicBrainz.hpp"

namespace atomicripper::metadata {

class CueToolsDB {
public:
    static MbResult lookup(const drive::TOC& toc);
};

} // namespace atomicripper::metadata
