#pragma once
#include "Drive.hpp"
#include <vector>

namespace atomicripper::drive {

// Discovers all optical drives present on the system.
class DriveEnumerator {
public:
    // Returns every optical drive found, regardless of whether a disc is inserted.
    static std::vector<Drive> enumerate();
};

} // namespace atomicripper::drive
