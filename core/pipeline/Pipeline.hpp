#pragma once
#include "../drive/TOC.hpp"
#include "../encode/IEncoder.hpp"
#include "../metadata/MusicBrainz.hpp"
#include "../rip/RipEngine.hpp"
#include "../verify/AccurateRip.hpp"
// #include "../upload/B2Uploader.hpp"   // uncomment to enable B2 upload support

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace atomicripper::pipeline {

// ---------------------------------------------------------------------------
// Pipeline states
// ---------------------------------------------------------------------------
enum class PipelineState {
    Idle,
    ReadingToc,
    FetchingMetadata,
    WaitingForRelease,   // onMetadataReady fired; waiting for selectRelease()
    Ripping,
    Verifying,
    Tagging,
    Complete,
    Cancelled,
    Error
};

// ---------------------------------------------------------------------------
// Info emitted when a single track finishes ripping + encoding
// ---------------------------------------------------------------------------
struct TrackDoneInfo {
    int                   trackNumber;
    uint32_t              crc32;
    int                   suspectSectors;
    int                   c2Sectors;
    std::filesystem::path outputPath;   // empty if this track failed
    bool                  ok;
};

// ---------------------------------------------------------------------------
// Typed callbacks — all optional (null-check before calling)
// ---------------------------------------------------------------------------
struct PipelineCallbacks {
    std::function<void(PipelineState)>                        onStateChanged;
    std::function<void(const drive::TOC&)>                    onTocRead;
    // onMetadataReady: caller MUST call selectRelease() or cancel()
    std::function<void(const metadata::MbResult&)>            onMetadataReady;
    std::function<void(int track, int total, uint32_t sectors)> onTrackStart;
    std::function<void(const rip::RipProgress&)>              onTrackProgress;
    std::function<void(const TrackDoneInfo&)>                 onTrackDone;
    std::function<void(const verify::ArDiscResult&)>          onVerifyDone;
    std::function<void(const verify::ArOffsetResult&)>        onOffsetDetected;
    std::function<void(int taggedCount)>                      onTagsDone;
    std::function<void()>                                     onComplete;
    std::function<void(const std::string& message)>           onError;
    std::function<void()>                                     onCancelled;

    // B2 upload callbacks — uncomment when upload support is enabled (see above)
    /*
    std::function<void(const std::string& b2Path, int index, int total)> onUploadProgress;
    std::function<void(const upload::B2UploadResult&)>                   onUploadDone;
    */
};

// ---------------------------------------------------------------------------
// Configuration for a single rip job
// ---------------------------------------------------------------------------
struct PipelineConfig {
    std::filesystem::path   outputDir;
    encode::Format          format           = encode::Format::FLAC;
    rip::RipSettings        ripSettings;
    encode::EncoderSettings encoderSettings;

    bool fetchMetadata     = true;   // query MusicBrainz after TOC read
    bool embedCoverArt     = true;   // download + embed front cover (FLAC only)
    bool verifyAccurateRip = true;   // run AccurateRip verification
    bool autoDetectOffset  = false;  // sweep ±1176 samples if no tracks match at offset=0
    bool writeTags         = true;   // write tags via TagLib (FLAC only)
    bool autoSelectRelease = false;  // pick the first release automatically
    bool writeCueSheet     = true;   // write a .cue file alongside the tracks
    bool writeRipLog       = true;   // write AtomicRipper.log proof/audit file
    bool writeUploadInfo   = true;   // write RED-style paste-ready upload fields
    bool ejectWhenDone     = false;  // eject disc tray after rip completes
    bool useManualMetadata = false;  // use GUI-entered album/track fields when no online release is selected
    metadata::MbRelease manualRelease;

    // -----------------------------------------------------------------------
    // B2 upload — send completed rip straight to the AtomicBlast B2 bucket
    // -----------------------------------------------------------------------
    // TO ENABLE:
    //   1. Uncomment #include "../upload/B2Uploader.hpp" at the top of this file
    //   2. Uncomment upload/B2Uploader.cpp in core/CMakeLists.txt
    //   3. Uncomment the two B2 callbacks below in PipelineCallbacks
    //   4. Uncomment step 11 in pipeline/Pipeline.cpp
    //   5. Set uploadToB2 = true and fill in b2Config with your credentials
    /*
    bool               uploadToB2 = false;  // set true to upload after tagging
    upload::B2Config   b2Config;            // credentials + bucket (see B2Uploader.hpp)
    */

    // Single-file mode: encode all audio tracks into one FLAC with an embedded
    // CUESHEET block and a companion .cue file (FLAC only; ignored for WAV).
    bool singleFile        = false;
};

// ---------------------------------------------------------------------------
// Pipeline — async orchestration of rip → encode → verify → tag
// ---------------------------------------------------------------------------
class Pipeline {
public:
    Pipeline(PipelineConfig config, PipelineCallbacks callbacks);
    ~Pipeline();

    // Start the pipeline in a background thread. Returns immediately.
    void start(const std::string& drivePath);

    // Call from inside the onMetadataReady callback (or any thread after it fires).
    // releaseIndex is a 0-based index into MbResult::releases.
    void selectRelease(int releaseIndex);

    // Request cancellation. The worker stops at the next safe checkpoint.
    void cancel();

    // Block the calling thread until the pipeline reaches a terminal state.
    void waitForCompletion();

    bool          isRunning() const;
    PipelineState state()     const;

private:
    void workerThread(std::string drivePath);
    void setState(PipelineState s);

    PipelineConfig    m_config;
    PipelineCallbacks m_cb;

    std::thread                m_thread;
    std::atomic<PipelineState> m_state{PipelineState::Idle};
    std::atomic<bool>          m_cancelled{false};

    // Release-selection rendezvous (condition variable)
    std::mutex              m_releaseMutex;
    std::condition_variable m_releaseCV;
    int                     m_selectedRelease = -1;
    bool                    m_releaseReady    = false;
};

} // namespace atomicripper::pipeline
