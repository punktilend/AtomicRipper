#include "RipEngine.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <winioctl.h>   // CTL_CODE, FILE_DEVICE_CONTROLLER, etc.
#  include <ntddscsi.h>   // SCSI_PASS_THROUGH_DIRECT, IOCTL_SCSI_PASS_THROUGH_DIRECT
#  include <cstddef>      // offsetof
static_assert(sizeof(HANDLE) == sizeof(void*),
    "HANDLE and void* must be the same size on this platform");
#endif

#include <algorithm>
#include <chrono>
#include <cstring>

namespace atomicripper::rip {

// ---------------------------------------------------------------------------
// CRC32 (ISO 3309 / zlib polynomial 0xEDB88320)
// ---------------------------------------------------------------------------
namespace {

uint32_t crc32Table[256];
bool     crc32TableReady = false;

void buildCRC32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32Table[i] = c;
    }
    crc32TableReady = true;
}

uint32_t computeCRC32(const uint8_t* data, size_t len) {
    if (!crc32TableReady) buildCRC32Table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = crc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

RipEngine::RipEngine(std::string drivePath, RipSettings settings)
    : m_drivePath(std::move(drivePath))
    , m_settings(settings)
{}

RipEngine::~RipEngine() {
    close();
}

// ===========================================================================
// Windows implementation
// ===========================================================================
#ifdef _WIN32

bool RipEngine::open() {
    close();

    // Drive path is "X:" — prepend UNC device prefix
    std::wstring devicePath = L"\\\\.\\" +
        std::wstring(m_drivePath.begin(), m_drivePath.end());

    HANDLE h = CreateFileW(
        devicePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (h == INVALID_HANDLE_VALUE)
        return false;

    m_handle = static_cast<void*>(h);
    return true;
}

void RipEngine::close() {
    if (m_handle && m_handle != reinterpret_cast<void*>(-1)) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}

bool RipEngine::isOpen() const {
    return m_handle != nullptr && m_handle != reinterpret_cast<void*>(-1);
}

// ---------------------------------------------------------------------------
// SCSI passthrough helpers
// ---------------------------------------------------------------------------
namespace {

// Layout for IOCTL_SCSI_PASS_THROUGH_DIRECT.
// SenseInfoOffset points to the sense[] array that follows the fixed struct.
struct SptiBuffer {
    SCSI_PASS_THROUGH_DIRECT spt;
    BYTE                     sense[32];
};

} // namespace

bool RipEngine::readSectors(uint32_t lba, int count, uint8_t* outBuf, bool withC2) {
    // Raw CD-DA: 2352 bytes/sector audio
    // With C2:   2352 + 294 bytes/sector (1 bit per audio byte = 294 bytes)
    const ULONG bytesPerSector = withC2 ? 2646u : 2352u;
    const ULONG totalBytes     = static_cast<ULONG>(count) * bytesPerSector;

    SptiBuffer buf{};
    buf.spt.Length             = sizeof(SCSI_PASS_THROUGH_DIRECT);
    buf.spt.CdbLength          = 12;
    buf.spt.SenseInfoLength    = static_cast<UCHAR>(sizeof(buf.sense));
    buf.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    buf.spt.DataTransferLength = totalBytes;
    buf.spt.TimeOutValue       = 30;   // seconds
    buf.spt.DataBuffer         = outBuf;
    buf.spt.SenseInfoOffset    = static_cast<ULONG>(offsetof(SptiBuffer, sense));

    // READ CD (0xBE) — MMC-5 §6.18
    //  Byte 0:   0xBE  Operation Code
    //  Byte 1:   Expected Sector Type = 001b (CD-DA) → bits 4:2 = 001 → 0x04
    //  Bytes 2-5: Starting LBA (big-endian)
    //  Bytes 6-8: Transfer Length in sectors (big-endian 24-bit)
    //  Byte 9:   Sub-header/User-Data/C2 flags
    //              bit 4 = User Data (2352 bytes of audio)
    //              bits 2:1 = Error Field (01b = C2 error flags, 294 bytes after audio)
    //  Byte 10:  Sub-channel data selection (0 = none)
    //  Byte 11:  Control

    buf.spt.Cdb[0]  = 0xBE;
    buf.spt.Cdb[1]  = 0x04;
    buf.spt.Cdb[2]  = static_cast<BYTE>((lba   >> 24) & 0xFF);
    buf.spt.Cdb[3]  = static_cast<BYTE>((lba   >> 16) & 0xFF);
    buf.spt.Cdb[4]  = static_cast<BYTE>((lba   >> 8 ) & 0xFF);
    buf.spt.Cdb[5]  = static_cast<BYTE>( lba          & 0xFF);
    buf.spt.Cdb[6]  = static_cast<BYTE>((count >> 16) & 0xFF);
    buf.spt.Cdb[7]  = static_cast<BYTE>((count >> 8 ) & 0xFF);
    buf.spt.Cdb[8]  = static_cast<BYTE>( count        & 0xFF);
    buf.spt.Cdb[9]  = withC2 ? BYTE(0x12) : BYTE(0x10);  // user data [+ C2 flags]
    buf.spt.Cdb[10] = 0x00;
    buf.spt.Cdb[11] = 0x00;

    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        static_cast<HANDLE>(m_handle),
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &buf, sizeof(buf),
        &buf, sizeof(buf),
        &returned,
        nullptr
    );

    return ok && buf.spt.ScsiStatus == 0;
}

// ---------------------------------------------------------------------------
// Capability probe
// ---------------------------------------------------------------------------

bool RipEngine::probeCapabilities() {
    if (!isOpen()) return false;

    // Try a C2-enabled read of sector 0.
    // If the SCSI command succeeds, the drive supports C2 error reporting.
    std::vector<uint8_t> testBuf(2646, 0);
    m_supportsC2 = readSectors(0, 1, testBuf.data(), /*withC2=*/true);

    // If C2 probe failed but user disabled C2, still succeed
    if (!m_supportsC2 && !m_settings.useC2Errors)
        return true;

    return true;
}

// ---------------------------------------------------------------------------
// Burst mode — single read, no verification
// ---------------------------------------------------------------------------

SectorResult RipEngine::ripSectorBurst(uint32_t lba) {
    SectorResult result;
    result.lba         = lba;
    result.confidence  = 0;
    result.hasC2Errors = false;
    result.retries     = 0;
    result.data.resize(2352, 0);

    const bool useC2 = m_settings.useC2Errors && m_supportsC2;
    std::vector<uint8_t> buf(useC2 ? 2646 : 2352, 0);

    if (readSectors(lba, 1, buf.data(), useC2)) {
        std::copy(buf.begin(), buf.begin() + 2352, result.data.begin());
        result.confidence = 1;

        if (useC2) {
            for (int i = 2352; i < 2646; ++i) {
                if (buf[i] != 0) { result.hasC2Errors = true; break; }
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Secure mode — multi-pass with majority vote
// ---------------------------------------------------------------------------

SectorResult RipEngine::ripSectorSecure(uint32_t lba) {
    SectorResult result;
    result.lba         = lba;
    result.confidence  = 0;
    result.hasC2Errors = false;
    result.retries     = 0;
    result.data.assign(2352, 0);

    const bool useC2          = m_settings.useC2Errors && m_supportsC2;
    const int  bytesPerRead   = useC2 ? 2646 : 2352;

    // Collect multiple reads of this sector (audio bytes only, 2352 per read)
    std::vector<std::vector<uint8_t>> reads;
    reads.reserve(static_cast<size_t>(m_settings.minMatches + m_settings.maxRetries));

    auto doRead = [&]() -> bool {
        std::vector<uint8_t> buf(bytesPerRead, 0);
        if (!readSectors(lba, 1, buf.data(), useC2))
            return false;

        if (useC2) {
            for (int i = 2352; i < 2646; ++i) {
                if (buf[i] != 0) { result.hasC2Errors = true; break; }
            }
            buf.resize(2352);  // keep only audio bytes
        }

        reads.push_back(std::move(buf));
        return true;
    };

    // Initial minimum passes
    for (int i = 0; i < m_settings.minMatches; ++i) {
        if (m_cancelled) return result;
        doRead();
    }

    // Fast-path: all initial reads agree
    auto allAgree = [&]() -> bool {
        if (reads.size() < 2) return false;
        for (size_t i = 1; i < reads.size(); ++i)
            if (reads[i] != reads[0]) return false;
        return true;
    };

    if (allAgree()) {
        result.data       = reads[0];
        result.confidence = static_cast<uint8_t>(
            std::min<size_t>(reads.size(), 255));
        return result;
    }

    // Retry loop — keep reading until majority consensus
    for (int retry = 0; retry < m_settings.maxRetries && !m_cancelled; ++retry) {
        ++result.retries;
        doRead();

        // Find the read that matches the most other reads (majority vote)
        int bestCount = 0;
        int bestIdx   = 0;
        for (size_t i = 0; i < reads.size(); ++i) {
            int count = 0;
            for (size_t j = 0; j < reads.size(); ++j)
                if (reads[i] == reads[j]) ++count;
            if (count > bestCount) {
                bestCount = count;
                bestIdx   = static_cast<int>(i);
            }
        }

        if (bestCount >= m_settings.minMatches) {
            result.data       = reads[static_cast<size_t>(bestIdx)];
            result.confidence = static_cast<uint8_t>(std::min(bestCount, 255));
            return result;
        }
    }

    // Could not reach consensus — take the most common read anyway (best effort)
    if (!reads.empty()) {
        result.data = reads[0];
    }
    result.confidence = 0;  // signal to caller that this sector is suspect
    return result;
}

// ---------------------------------------------------------------------------
// ripTrack — iterate sectors, assemble track, apply drive offset correction
// ---------------------------------------------------------------------------

TrackRipResult RipEngine::ripTrack(const drive::TrackInfo& track,
                                    int                    trackNumberHint,
                                    ProgressCallback       onProgress) {
    TrackRipResult result;

    if (!isOpen() || m_cancelled.load()) {
        result.cancelled = m_cancelled.load();
        return result;
    }

    // Drive offset: measured in samples. Each stereo 16-bit sample = 4 bytes.
    // We read extra sectors around the track boundary so we can shift the buffer.
    const int  byteOffset  = m_settings.driveOffset * 4;
    // Number of extra sectors needed to cover the byte shift (at least 1 for safety)
    const int  extraNeeded = (std::abs(byteOffset) + 2351) / 2352 + 1;

    // Adjusted LBA range
    int32_t  readStartLBA = static_cast<int32_t>(track.lba);
    uint32_t readCount    = track.sectorCount + static_cast<uint32_t>(extraNeeded);

    if (byteOffset < 0) {
        // Negative offset: read extra sectors before the track start
        readStartLBA = std::max<int32_t>(0, readStartLBA - extraNeeded);
        readCount   += static_cast<uint32_t>(extraNeeded);
    }

    const uint32_t trackSectorCount = track.sectorCount;
    result.sectors.reserve(trackSectorCount);

    // Raw buffer: all sectors read (includes extra for offset correction)
    std::vector<uint8_t> rawBuf;
    rawBuf.reserve(static_cast<size_t>(readCount) * 2352);

    auto    startTime    = std::chrono::steady_clock::now();
    int     totalRetries = 0;
    uint32_t trackSectorsProcessed = 0;

    // Sector index window: which reads correspond to actual track sectors
    const uint32_t trackSectorStart = (byteOffset < 0)
        ? static_cast<uint32_t>(extraNeeded)
        : 0u;
    const uint32_t trackSectorEnd   = trackSectorStart + trackSectorCount;

    for (uint32_t i = 0; i < readCount && !m_cancelled.load(); ++i) {
        const uint32_t lba = static_cast<uint32_t>(readStartLBA) + i;

        SectorResult sr;
        switch (m_settings.mode) {
        case RipMode::Burst:
            sr = ripSectorBurst(lba);
            break;
        case RipMode::Secure:
        case RipMode::Paranoia:
            sr = ripSectorSecure(lba);
            break;
        }

        totalRetries += sr.retries;

        // Append audio bytes to raw buffer (zero-fill on read failure)
        if (sr.data.size() == 2352)
            rawBuf.insert(rawBuf.end(), sr.data.begin(), sr.data.end());
        else
            rawBuf.insert(rawBuf.end(), 2352, uint8_t{0});

        // Record quality info only for actual track sectors
        if (i >= trackSectorStart && i < trackSectorEnd) {
            result.sectors.push_back(std::move(sr));
            ++trackSectorsProcessed;
        }

        // Emit progress ~every 10 sectors
        if (onProgress && (i % 10 == 0 || i == readCount - 1)) {
            auto   now     = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            float  speedX  = (elapsed > 0.01)
                ? static_cast<float>((i + 1) * 2352.0 / (elapsed * 150.0 * 1024.0))
                : 0.0f;

            RipProgress prog;
            prog.currentTrack  = trackNumberHint;
            prog.currentSector = trackSectorsProcessed;
            prog.totalSectors  = trackSectorCount;
            prog.speedX        = speedX;
            prog.totalRetries  = totalRetries;
            onProgress(prog);
        }
    }

    if (m_cancelled.load()) {
        result.cancelled = true;
        return result;
    }

    // Apply drive offset: extract the correct byte window from rawBuf
    const size_t trackBytes = static_cast<size_t>(trackSectorCount) * 2352;

    size_t windowStart;
    if (byteOffset >= 0) {
        // Positive offset: skip byteOffset bytes from the start of rawBuf
        windowStart = static_cast<size_t>(byteOffset);
    } else {
        // Negative offset: we read extra sectors at the beginning.
        // The track data begins at (extraNeeded * 2352 + byteOffset) bytes in.
        const size_t extraBytes = static_cast<size_t>(extraNeeded) * 2352;
        windowStart = extraBytes + static_cast<size_t>(byteOffset);  // byteOffset is negative
    }

    if (rawBuf.size() >= windowStart + trackBytes) {
        result.data.assign(
            rawBuf.begin() + static_cast<ptrdiff_t>(windowStart),
            rawBuf.begin() + static_cast<ptrdiff_t>(windowStart + trackBytes));
    } else {
        // Fallback: just take what we have (shouldn't normally happen)
        result.data = rawBuf;
        result.data.resize(trackBytes, 0);
    }

    result.crc32 = computeCRC32(result.data.data(), result.data.size());
    result.ok    = true;
    return result;
}

// ===========================================================================
// Linux / macOS stubs — will be implemented in a later phase
// ===========================================================================
#else

bool RipEngine::open()              { return false; }
void RipEngine::close()             {}
bool RipEngine::isOpen() const      { return false; }
bool RipEngine::probeCapabilities() { return false; }

SectorResult RipEngine::ripSectorBurst(uint32_t lba) {
    SectorResult r;
    r.lba = lba;
    r.data.assign(2352, 0);
    r.confidence  = 0;
    r.hasC2Errors = false;
    r.retries     = 0;
    return r;
}

SectorResult RipEngine::ripSectorSecure(uint32_t lba) {
    return ripSectorBurst(lba);
}

TrackRipResult RipEngine::ripTrack(const drive::TrackInfo&, int, ProgressCallback) {
    return {};
}

#endif

} // namespace atomicripper::rip
