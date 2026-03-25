#include "WavEncoder.hpp"
#include <cstring>

namespace atomicripper::encode {

// ---------------------------------------------------------------------------
// Little-endian write helpers
// ---------------------------------------------------------------------------
namespace {
void writeU16LE(FILE* fp, uint16_t v) {
    const uint8_t b[2] = { uint8_t(v & 0xFF), uint8_t(v >> 8) };
    fwrite(b, 1, 2, fp);
}
void writeU32LE(FILE* fp, uint32_t v) {
    const uint8_t b[4] = {
        uint8_t(v        & 0xFF), uint8_t((v >>  8) & 0xFF),
        uint8_t((v >> 16) & 0xFF), uint8_t((v >> 24) & 0xFF)
    };
    fwrite(b, 1, 4, fp);
}
} // namespace

// ---------------------------------------------------------------------------
WavEncoder::WavEncoder()  = default;
WavEncoder::~WavEncoder() {
    if (m_opened && !m_finalized)
        finalize();
}

void WavEncoder::setTag(const std::string& /*key*/, const std::string& /*value*/) {
    // WAV has no standard metadata block — silently ignored.
}

std::string WavEncoder::lastError() const { return m_lastError; }

// ---------------------------------------------------------------------------
// writeHeaders — writes the 44-byte RIFF/fmt /data preamble at the
// current file position.  dataSize=0 is written as a placeholder
// (patched in finalize() when the true size is known).
// ---------------------------------------------------------------------------
void WavEncoder::writeHeaders(uint32_t dataSize) {
    const uint16_t channels       = static_cast<uint16_t>(m_channels);
    const uint16_t bitsPerSample  = static_cast<uint16_t>(m_bitsPerSample);
    const uint32_t sampleRate     = 44100;
    const uint16_t blockAlign     = static_cast<uint16_t>(channels * bitsPerSample / 8);
    const uint32_t byteRate       = sampleRate * blockAlign;

    // RIFF chunk
    fwrite("RIFF", 1, 4, m_fp);
    writeU32LE(m_fp, dataSize == 0 ? 0 : dataSize + 36); // file size − 8
    fwrite("WAVE", 1, 4, m_fp);

    // fmt  chunk
    fwrite("fmt ", 1, 4, m_fp);
    writeU32LE(m_fp, 16);            // chunk size (PCM = no extra bytes)
    writeU16LE(m_fp, 1);             // PCM format
    writeU16LE(m_fp, channels);
    writeU32LE(m_fp, sampleRate);
    writeU32LE(m_fp, byteRate);
    writeU16LE(m_fp, blockAlign);
    writeU16LE(m_fp, bitsPerSample);

    // data chunk header
    fwrite("data", 1, 4, m_fp);
    writeU32LE(m_fp, dataSize);
}

// ---------------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------------
bool WavEncoder::open(const std::filesystem::path& outputPath,
                       const EncoderSettings& settings) {
    m_lastError.clear();
    m_channels      = settings.channels;
    m_bitsPerSample = settings.bitsPerSample;
    m_dataBytes     = 0;

    m_fp = fopen(outputPath.string().c_str(), "wb");
    if (!m_fp) {
        m_lastError = "cannot open file: " + outputPath.string();
        return false;
    }

    // If total samples are known, write correct sizes immediately.
    // Otherwise write placeholder zeros and patch in finalize().
    const uint32_t knownDataBytes = settings.totalSamples > 0
        ? static_cast<uint32_t>(settings.totalSamples *
              static_cast<uint64_t>(m_channels) *
              static_cast<uint64_t>(m_bitsPerSample / 8))
        : 0u;

    writeHeaders(knownDataBytes);

    m_opened    = true;
    m_finalized = false;
    return true;
}

// ---------------------------------------------------------------------------
// writeSamples() — convert int32_t → little-endian int16_t and write
// ---------------------------------------------------------------------------
bool WavEncoder::writeSamples(std::span<const int32_t> samples) {
    if (!m_opened || m_finalized) {
        m_lastError = "encoder not open";
        return false;
    }
    if (samples.empty()) return true;

    // CD audio: 16-bit samples clamped to [-32768, 32767].
    // The pipeline always passes values already in this range (sourced from
    // cdBytesToSamples which preserves the original 16-bit integers), so the
    // cast is safe.  We write them as little-endian int16_t.
    for (const int32_t s : samples) {
        const int16_t s16 = static_cast<int16_t>(s);
        const uint8_t b[2] = { uint8_t(s16 & 0xFF), uint8_t(uint16_t(s16) >> 8) };
        fwrite(b, 1, 2, m_fp);
    }

    m_dataBytes += static_cast<uint32_t>(samples.size() * 2u); // 2 bytes per sample
    return true;
}

// ---------------------------------------------------------------------------
// finalize() — patch RIFF + data chunk sizes if they were written as zeros
// ---------------------------------------------------------------------------
bool WavEncoder::finalize() {
    if (!m_opened || m_finalized) {
        m_lastError = "encoder not open";
        return false;
    }
    m_finalized = true;

    // Seek back and overwrite the header with the real sizes.
    // (If totalSamples was known at open() the sizes are already correct,
    //  but patching again is harmless and keeps the code simple.)
    if (fseek(m_fp, 0, SEEK_SET) != 0) {
        m_lastError = "fseek failed when patching WAV header";
        fclose(m_fp);
        m_fp = nullptr;
        return false;
    }

    writeHeaders(m_dataBytes);

    fclose(m_fp);
    m_fp = nullptr;
    return true;
}

} // namespace atomicripper::encode
