#include "FlacEncoder.hpp"

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cassert>
#include <map>
#include <string>
#include <vector>

namespace atomicripper::encode {

// ---------------------------------------------------------------------------
// Pimpl — keeps libFLAC headers out of FlacEncoder.hpp
// ---------------------------------------------------------------------------

struct FlacEncoder::Impl {
    FLAC__StreamEncoder*               encoder     = nullptr;
    std::map<std::string, std::string> tags;
    std::vector<FLAC__StreamMetadata*> metaObjects; // owned; freed in dtor
    std::string                        lastError;
    bool                               opened      = false;
    bool                               finalized   = false;

    // Cover art — set via setPicture() before open()
    std::vector<uint8_t> pictureData;
    std::string          pictureMime;

    ~Impl() {
        // Finish encoding if open but not yet finalized
        if (encoder) {
            if (opened && !finalized)
                FLAC__stream_encoder_finish(encoder);
            FLAC__stream_encoder_delete(encoder);
            encoder = nullptr;
        }
        for (auto* m : metaObjects)
            FLAC__metadata_object_delete(m);
        metaObjects.clear();
    }
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

FlacEncoder::FlacEncoder()  : m_impl(std::make_unique<Impl>()) {}
FlacEncoder::~FlacEncoder() = default;

// ---------------------------------------------------------------------------
// Tag management
// ---------------------------------------------------------------------------

void FlacEncoder::setTag(const std::string& key, const std::string& value) {
    // Store uppercase key per Vorbis Comment convention
    std::string upper = key;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    m_impl->tags[upper] = value;
}

void FlacEncoder::clearTags() {
    m_impl->tags.clear();
}

void FlacEncoder::setPicture(const std::vector<uint8_t>& data,
                              const std::string& mimeType) {
    m_impl->pictureData = data;
    m_impl->pictureMime = mimeType;
}

std::string FlacEncoder::lastError() const {
    return m_impl->lastError;
}

// ---------------------------------------------------------------------------
// open() — configure encoder, build metadata chain, init to file
// ---------------------------------------------------------------------------

bool FlacEncoder::open(const std::filesystem::path& outputPath,
                        const EncoderSettings& settings) {
    m_impl->lastError.clear();

    m_impl->encoder = FLAC__stream_encoder_new();
    if (!m_impl->encoder) {
        m_impl->lastError = "FLAC__stream_encoder_new failed";
        return false;
    }

    FLAC__stream_encoder_set_channels(
        m_impl->encoder, static_cast<unsigned>(settings.channels));
    FLAC__stream_encoder_set_bits_per_sample(
        m_impl->encoder, static_cast<unsigned>(settings.bitsPerSample));
    FLAC__stream_encoder_set_sample_rate(
        m_impl->encoder, static_cast<unsigned>(settings.sampleRate));
    FLAC__stream_encoder_set_compression_level(
        m_impl->encoder, static_cast<unsigned>(
            std::clamp(settings.compressionLevel, 0, 8)));

    if (settings.totalSamples > 0)
        FLAC__stream_encoder_set_total_samples_estimate(
            m_impl->encoder, settings.totalSamples);

    // -- Build metadata chain -----------------------------------------------

    // 1. Vorbis Comment (tags)
    auto* vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    if (vc) {
        for (const auto& [key, value] : m_impl->tags) {
            FLAC__StreamMetadata_VorbisComment_Entry entry{};
            if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
                    &entry, key.c_str(), value.c_str())) {
                FLAC__metadata_object_vorbiscomment_append_comment(
                    vc, entry, /*copy=*/false);
            }
        }
        m_impl->metaObjects.push_back(vc);
    }

    // 2. PICTURE block — front cover art (optional)
    if (!m_impl->pictureData.empty()) {
        auto* pic = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);
        if (pic) {
            pic->data.picture.type = FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER;

            // MIME type (copy=true so libFLAC owns the memory)
            std::string mime = m_impl->pictureMime.empty()
                ? "image/jpeg" : m_impl->pictureMime;
            FLAC__metadata_object_picture_set_mime_type(
                pic, const_cast<char*>(mime.c_str()), /*copy=*/true);

            // Image data (copy=true)
            FLAC__metadata_object_picture_set_data(
                pic,
                const_cast<FLAC__byte*>(
                    reinterpret_cast<const FLAC__byte*>(m_impl->pictureData.data())),
                static_cast<FLAC__uint32>(m_impl->pictureData.size()),
                /*copy=*/true);

            // Width/height/colour depth/indexed colours — 0 = unknown (valid)
            pic->data.picture.width       = 0;
            pic->data.picture.height      = 0;
            pic->data.picture.depth       = 0;
            pic->data.picture.colors      = 0;

            m_impl->metaObjects.push_back(pic);
        }
    }

    // 3. Seek table — one seek point every 10 seconds
    //    Gives players fast random access without being too large.
    auto* seekTable = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
    if (seekTable) {
        if (settings.totalSamples > 0) {
            const FLAC__uint32 spacing = static_cast<FLAC__uint32>(
                settings.sampleRate * 10u);  // 10-second intervals
            FLAC__metadata_object_seektable_template_append_spaced_points_by_samples(
                seekTable, spacing, settings.totalSamples);
            FLAC__metadata_object_seektable_template_sort(seekTable, /*compact=*/true);
        }
        m_impl->metaObjects.push_back(seekTable);
    }

    // 3. Padding — allows tag editing without rewriting the audio stream
    auto* padding = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
    if (padding) {
        padding->length = 8192;
        m_impl->metaObjects.push_back(padding);
    }

    if (!m_impl->metaObjects.empty()) {
        FLAC__stream_encoder_set_metadata(
            m_impl->encoder,
            m_impl->metaObjects.data(),
            static_cast<unsigned>(m_impl->metaObjects.size()));
    }

    // -- Initialise to file -------------------------------------------------
    const FLAC__StreamEncoderInitStatus status =
        FLAC__stream_encoder_init_file(
            m_impl->encoder,
            outputPath.string().c_str(),
            /*progress_callback=*/nullptr,
            /*client_data=*/nullptr);

    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        m_impl->lastError = FLAC__StreamEncoderInitStatusString[status];
        return false;
    }

    m_impl->opened = true;
    return true;
}

// ---------------------------------------------------------------------------
// writeSamples() — feed interleaved int32_t PCM to the encoder
// ---------------------------------------------------------------------------

bool FlacEncoder::writeSamples(std::span<const int32_t> samples) {
    if (!m_impl->opened || m_impl->finalized) {
        m_impl->lastError = "encoder not open";
        return false;
    }
    if (samples.empty()) return true;

    // FLAC__stream_encoder_process_interleaved counts stereo FRAMES,
    // not individual channel samples.
    const unsigned frames = static_cast<unsigned>(
        samples.size() / static_cast<size_t>(2));  // always 2 channels for CD

    const FLAC__bool ok = FLAC__stream_encoder_process_interleaved(
        m_impl->encoder,
        reinterpret_cast<const FLAC__int32*>(samples.data()),
        frames);

    if (!ok) {
        m_impl->lastError = FLAC__StreamEncoderStateString[
            FLAC__stream_encoder_get_state(m_impl->encoder)];
    }
    return ok != 0;
}

// ---------------------------------------------------------------------------
// finalize() — flush, write updated seek table + metadata, close file
// ---------------------------------------------------------------------------

bool FlacEncoder::finalize() {
    if (!m_impl->opened || m_impl->finalized) {
        m_impl->lastError = "encoder not open";
        return false;
    }
    m_impl->finalized = true;

    const int ok = FLAC__stream_encoder_finish(m_impl->encoder);
    if (!ok) {
        m_impl->lastError = FLAC__StreamEncoderStateString[
            FLAC__stream_encoder_get_state(m_impl->encoder)];
    }
    return ok != 0;
}

// ---------------------------------------------------------------------------
// cdBytesToSamples — raw CD bytes → interleaved int32_t PCM
// ---------------------------------------------------------------------------

void cdBytesToSamples(const uint8_t* bytes, size_t byteCount,
                      std::vector<int32_t>& outSamples) {
    // CD-DA: 16-bit little-endian signed stereo PCM
    // 2 bytes per sample, interleaved L R L R …
    const size_t sampleCount = byteCount / 2;
    outSamples.resize(sampleCount);

    for (size_t i = 0; i < sampleCount; ++i) {
        // Reconstruct signed 16-bit value from two little-endian bytes
        const int16_t s = static_cast<int16_t>(
            static_cast<uint16_t>(bytes[i * 2]) |
            (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8));
        outSamples[i] = static_cast<int32_t>(s);
    }
}

} // namespace atomicripper::encode
