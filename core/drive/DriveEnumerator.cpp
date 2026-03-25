#include "DriveEnumerator.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace atomicripper::drive {

#ifdef _WIN32

std::vector<Drive> DriveEnumerator::enumerate() {
    std::vector<Drive> drives;

    // GetLogicalDrives() returns a bitmask: bit 0 = A:, bit 1 = B:, bit 2 = C:, …
    DWORD mask = GetLogicalDrives();

    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;

        char letter = static_cast<char>('A' + i);
        std::string root = { letter, ':', '\\' };

        if (GetDriveTypeA(root.c_str()) != DRIVE_CDROM)
            continue;

        // Query the volume label / description for the drive
        char volumeName[256]  = {};
        char fsName[64]       = {};
        GetVolumeInformationA(root.c_str(), volumeName, sizeof(volumeName),
                              nullptr, nullptr, nullptr, fsName, sizeof(fsName));

        // Build a friendly description: e.g. "D: [MY_CD]" or just "D:"
        std::string path = { letter, ':' };
        std::string desc = path;
        if (volumeName[0] != '\0')
            desc += " [" + std::string(volumeName) + "]";

        drives.emplace_back(path, desc);
    }

    return drives;
}

// ---------------------------------------------------------------------------
// Linux / macOS stub
// ---------------------------------------------------------------------------
#else

std::vector<Drive> DriveEnumerator::enumerate() {
    // TODO: use libcdio or scan /dev/sr* on Linux, IOKit on macOS
    return {};
}

#endif

} // namespace atomicripper::drive
