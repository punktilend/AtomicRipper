#pragma once
#include "../encode/IEncoder.hpp"
#include <filesystem>
#include <functional>
#include <string>

namespace atomicripper::pipeline {

// ---------------------------------------------------------------------------
// State machine states
// ---------------------------------------------------------------------------
enum class PipelineState {
    Idle,
    DriveDetected,    // drive found, waiting for disc
    TocRead,          // TOC successfully read
    MetadataFetching, // querying MusicBrainz / Discogs
    MetadataReady,    // metadata available, waiting for user confirmation
    Ripping,          // actively reading sectors
    Encoding,         // writing audio to file(s)
    Verifying,        // AccurateRip lookup
    Tagging,          // writing tags via TagLib
    Complete,
    Cancelled,
    Error
};

// ---------------------------------------------------------------------------
// Job configuration
// ---------------------------------------------------------------------------
struct PipelineConfig {
    std::filesystem::path outputDirectory;

    // Naming template tokens: {artist} {albumartist} {album} {year}
    //                         {track:02d} {title} {disc}
    std::string namingTemplate = "{albumartist}/{year} - {album}/{track:02d} - {title}";

    encode::EncoderSettings encoderSettings;

    bool ejectWhenDone = false;
    bool autoStart     = false; // start ripping without waiting for user confirmation
};

// ---------------------------------------------------------------------------
// Event callbacks (all optional — set the ones you care about)
// ---------------------------------------------------------------------------
struct PipelineEvents {
    std::function<void(PipelineState oldState, PipelineState newState)> onStateChanged;
    std::function<void(int track, int totalTracks, float percent)>      onProgress;
    std::function<void(int track, uint8_t confidence)>                  onTrackVerified;
    std::function<void(const std::string& message)>                     onLog;
    std::function<void(const std::string& error)>                       onError;
};

// ---------------------------------------------------------------------------
// Pipeline — implemented in Phase 5 (after all core layers exist)
// ---------------------------------------------------------------------------
class Pipeline {
public:
    Pipeline(PipelineConfig config, PipelineEvents events);

    void start(const std::string& drivePath);
    void cancel();

    PipelineState state() const;
};

} // namespace atomicripper::pipeline
