#include "Drive.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winioctl.h>
#  include <ntddcdrm.h>   // IOCTL_CDROM_*, CDROM_TOC, TRACK_DATA
#endif

namespace atomicripper::drive {

Drive::Drive(std::string path, std::string description)
    : m_path(std::move(path))
    , m_description(std::move(description))
{}

// ---------------------------------------------------------------------------
// Windows implementation
// ---------------------------------------------------------------------------
#ifdef _WIN32

namespace {

// Open the drive for IOCTL access.
// path is expected to be "X:" (no trailing backslash).
HANDLE openDrive(const std::string& path) {
    std::wstring devicePath = L"\\\\.\\" +
        std::wstring(path.begin(), path.end());
    return CreateFileW(
        devicePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
}

// Convert MSF (minute/second/frame) to LBA.
// Windows CDROM_TOC stores addresses as [reserved, M, S, F] in MSF format.
// LBA = (M*60 + S)*75 + F - 150   (the -150 is the standard 2-second pregap)
constexpr uint32_t msfToLBA(uint8_t m, uint8_t s, uint8_t f) {
    return static_cast<uint32_t>(m * 60 + s) * 75 + f - 150;
}

} // anonymous namespace

DriveStatus Drive::status() const {
    HANDLE hDrive = openDrive(m_path);
    if (hDrive == INVALID_HANDLE_VALUE)
        return DriveStatus::Error;

    // RAII close
    struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } g{hDrive};

    // IOCTL_STORAGE_CHECK_VERIFY returns ERROR_NOT_READY if no disc is present
    DWORD bytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_CHECK_VERIFY,
                         nullptr, 0, nullptr, 0, &bytes, nullptr))
    {
        DWORD err = GetLastError();
        if (err == ERROR_NOT_READY || err == ERROR_NO_MEDIA_IN_DRIVE)
            return DriveStatus::Empty;
        return DriveStatus::NotReady;
    }
    return DriveStatus::Ready;
}

std::optional<TOC> Drive::readTOC() const {
    HANDLE hDrive = openDrive(m_path);
    if (hDrive == INVALID_HANDLE_VALUE)
        return std::nullopt;

    struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } g{hDrive};

    CDROM_TOC cdTOC{};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDrive, IOCTL_CDROM_READ_TOC,
                         nullptr, 0,
                         &cdTOC, sizeof(cdTOC),
                         &bytesReturned, nullptr))
        return std::nullopt;

    TOC toc;
    toc.firstTrack = cdTOC.FirstTrack;
    toc.lastTrack  = cdTOC.LastTrack;

    // TrackData entries: [FirstTrack … LastTrack] then lead-out (0xAA)
    int numEntries = cdTOC.LastTrack - cdTOC.FirstTrack + 2; // +1 for lead-out

    for (int i = 0; i < numEntries && i < MAXIMUM_NUMBER_TRACKS; ++i) {
        const TRACK_DATA& td = cdTOC.TrackData[i];
        uint32_t lba = msfToLBA(td.Address[1], td.Address[2], td.Address[3]);

        if (td.TrackNumber == 0xAA) {
            toc.leadOutLBA = lba;
        } else {
            TrackInfo track;
            track.number  = td.TrackNumber;
            track.isAudio = !(td.Control & 0x04); // Control bit 2: 0=audio, 1=data
            track.lba     = lba;
            // sectorCount filled in below after we have all LBAs
            toc.tracks.push_back(std::move(track));
        }
    }

    // Calculate sector counts now that all LBAs are known
    for (size_t i = 0; i < toc.tracks.size(); ++i) {
        uint32_t nextLBA = (i + 1 < toc.tracks.size())
            ? toc.tracks[i + 1].lba
            : toc.leadOutLBA;
        toc.tracks[i].sectorCount = nextLBA - toc.tracks[i].lba;
    }

    return toc;
}

bool Drive::eject() const {
    HANDLE hDrive = openDrive(m_path);
    if (hDrive == INVALID_HANDLE_VALUE)
        return false;

    struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } g{hDrive};

    DWORD bytes = 0;
    return DeviceIoControl(hDrive, IOCTL_STORAGE_EJECT_MEDIA,
                           nullptr, 0, nullptr, 0, &bytes, nullptr) != 0;
}

// ---------------------------------------------------------------------------
// Linux / macOS stub — will be implemented in a future phase
// ---------------------------------------------------------------------------
#else

DriveStatus Drive::status() const       { return DriveStatus::Error; }
std::optional<TOC> Drive::readTOC() const { return std::nullopt; }
bool Drive::eject() const               { return false; }

#endif

} // namespace atomicripper::drive
