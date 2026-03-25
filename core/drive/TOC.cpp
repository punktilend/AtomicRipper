#include "TOC.hpp"
#include <algorithm>

namespace atomicripper::drive {

int TOC::audioTrackCount() const {
    return static_cast<int>(
        std::count_if(tracks.begin(), tracks.end(),
                      [](const TrackInfo& t) { return t.isAudio; }));
}

} // namespace atomicripper::drive
