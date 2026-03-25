#pragma once
#include "TOC.hpp"
#include <optional>
#include <string>

namespace atomicripper::drive {

enum class DriveStatus {
    Ready,     // disc present and readable
    Empty,     // tray empty or no disc
    NotReady,  // drive busy / transitioning
    Error      // could not communicate with drive
};

// Represents one physical optical drive on the system.
// Platform-specific work is hidden in Drive.cpp behind #ifdef guards.
class Drive {
public:
    Drive(std::string path, std::string description);

    const std::string& path()        const { return m_path; }
    const std::string& description() const { return m_description; }

    DriveStatus          status() const;
    bool                 isReady() const { return status() == DriveStatus::Ready; }

    // Returns nullopt if no disc is present or TOC cannot be read
    std::optional<TOC>   readTOC() const;

    bool eject() const;

private:
    std::string m_path;         // e.g. "D:" on Windows, "/dev/sr0" on Linux
    std::string m_description;  // friendly name from OS
};

} // namespace atomicripper::drive
