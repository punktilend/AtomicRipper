#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "Pipeline.hpp"

#include "../drive/Drive.hpp"
#include "../encode/FlacEncoder.hpp"
#include "../encode/WavEncoder.hpp"
#include "../metadata/CoverArt.hpp"
#include "../metadata/CueSheet.hpp"
#include "../metadata/DiscId.hpp"
#include "../metadata/TagWriter.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

namespace atomicripper::pipeline {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

std::string sanitise(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<'  || c == '>' || c == '|')
            out += '_';
        else
            out += static_cast<char>(c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    return out.empty() ? "_" : out;
}

// Build a zero-padded track filename: "01 - Title.flac"
std::string makeTrackName(int number, const std::string& title, const char* ext) {
    char num[8];
    std::snprintf(num, sizeof(num), "%02d", number);
    if (title.empty())
        return std::string(num) + "." + ext;
    return std::string(num) + " - " + sanitise(title) + "." + ext;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
Pipeline::Pipeline(PipelineConfig config, PipelineCallbacks callbacks)
    : m_config(std::move(config))
    , m_cb(std::move(callbacks))
{}

Pipeline::~Pipeline() {
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Pipeline::start(const std::string& drivePath) {
    assert(!isRunning() && "Pipeline::start called while already running");
    m_cancelled      = false;
    m_releaseReady   = false;
    m_selectedRelease = -1;
    setState(PipelineState::ReadingToc);
    m_thread = std::thread([this, drivePath]() { workerThread(drivePath); });
}

void Pipeline::selectRelease(int releaseIndex) {
    {
        std::lock_guard<std::mutex> lk(m_releaseMutex);
        m_selectedRelease = releaseIndex;
        m_releaseReady    = true;
    }
    m_releaseCV.notify_one();
}

void Pipeline::cancel() {
    m_cancelled = true;
    // Unblock selectRelease() wait if pipeline is sitting in WaitingForRelease
    {
        std::lock_guard<std::mutex> lk(m_releaseMutex);
        m_releaseReady = true;
    }
    m_releaseCV.notify_one();
}

void Pipeline::waitForCompletion() {
    if (m_thread.joinable())
        m_thread.join();
}

bool Pipeline::isRunning() const {
    const auto s = m_state.load();
    return s != PipelineState::Idle
        && s != PipelineState::Complete
        && s != PipelineState::Cancelled
        && s != PipelineState::Error;
}

PipelineState Pipeline::state() const { return m_state.load(); }

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
void Pipeline::setState(PipelineState s) {
    m_state = s;
    if (m_cb.onStateChanged) m_cb.onStateChanged(s);
}

// ---------------------------------------------------------------------------
// Worker thread — full rip orchestration
// ---------------------------------------------------------------------------
void Pipeline::workerThread(std::string drivePath) {
    // ------------------------------------------------------------------
    // 1.  Open drive & read TOC
    // ------------------------------------------------------------------
    drive::Drive drv(drivePath, drivePath);
    if (!drv.isReady()) {
        setState(PipelineState::Error);
        if (m_cb.onError) m_cb.onError("Drive " + drivePath + " is not ready.");
        return;
    }

    auto tocOpt = drv.readTOC();
    if (!tocOpt || !tocOpt->isValid()) {
        setState(PipelineState::Error);
        if (m_cb.onError) m_cb.onError("Failed to read TOC from " + drivePath + ".");
        return;
    }
    const drive::TOC toc = *tocOpt;
    if (m_cb.onTocRead) m_cb.onTocRead(toc);

    if (m_cancelled) { setState(PipelineState::Cancelled); if (m_cb.onCancelled) m_cb.onCancelled(); return; }

    // ------------------------------------------------------------------
    // 2.  Ensure output directory exists
    // ------------------------------------------------------------------
    std::error_code ec;
    std::filesystem::create_directories(m_config.outputDir, ec);
    if (ec) {
        setState(PipelineState::Error);
        if (m_cb.onError) m_cb.onError("Cannot create output directory: " + ec.message());
        return;
    }

    // ------------------------------------------------------------------
    // 3.  MusicBrainz metadata (optional)
    // ------------------------------------------------------------------
    metadata::MbResult mbResult;
    int selectedRelease = -1;  // -1 = no metadata

    if (m_config.fetchMetadata) {
        setState(PipelineState::FetchingMetadata);

        std::string discId = metadata::DiscId::calculate(toc);
        mbResult = metadata::MusicBrainz::lookup(discId);

        if (mbResult.ok && !mbResult.releases.empty()) {
            if (m_config.autoSelectRelease) {
                selectedRelease = 0;
            } else {
                // Fire callback and wait for selectRelease() or cancel()
                setState(PipelineState::WaitingForRelease);
                if (m_cb.onMetadataReady) m_cb.onMetadataReady(mbResult);

                std::unique_lock<std::mutex> lk(m_releaseMutex);
                m_releaseCV.wait(lk, [this]{ return m_releaseReady; });
                selectedRelease = m_selectedRelease;
            }
        }
        // If lookup failed or no releases, selectedRelease stays -1 (tag-less rip)
    }

    if (m_cancelled) { setState(PipelineState::Cancelled); if (m_cb.onCancelled) m_cb.onCancelled(); return; }

    // ------------------------------------------------------------------
    // 3b. Cover art download (after release is chosen, before ripping)
    // ------------------------------------------------------------------
    metadata::CoverArtResult coverArt;
    if (m_config.fetchMetadata && m_config.embedCoverArt &&
        selectedRelease >= 0 &&
        selectedRelease < static_cast<int>(mbResult.releases.size())) {
        const std::string& releaseId = mbResult.releases[
            static_cast<size_t>(selectedRelease)].id;
        coverArt = metadata::CoverArt::fetchFront(releaseId);
        if (!coverArt.ok && m_cb.onError)
            m_cb.onError("Cover art: " + coverArt.error);  // non-fatal
    }

    // ------------------------------------------------------------------
    // 4.  Open rip engine
    // ------------------------------------------------------------------
    setState(PipelineState::Ripping);

    rip::RipEngine engine(drivePath, m_config.ripSettings);
    if (!engine.open()) {
        setState(PipelineState::Error);
        if (m_cb.onError) m_cb.onError("Could not open drive for SCSI access. Try running as Administrator.");
        return;
    }
    engine.probeCapabilities();

    const int totalAudio = toc.audioTrackCount();
    const bool isFlac    = (m_config.format == encode::Format::FLAC);
    const char* ext      = isFlac ? "flac" : "wav";

    // Collect track titles for filename generation (from MB if available)
    const metadata::MbRelease* release = nullptr;
    if (selectedRelease >= 0 && selectedRelease < static_cast<int>(mbResult.releases.size()))
        release = &mbResult.releases[static_cast<size_t>(selectedRelease)];

    std::vector<std::vector<uint8_t>> allTrackPcm;   // for AccurateRip
    std::vector<std::filesystem::path> writtenFiles;  // for tagging
    allTrackPcm.reserve(static_cast<size_t>(totalAudio));
    writtenFiles.reserve(static_cast<size_t>(totalAudio));

    // ------------------------------------------------------------------
    // 5.  Rip loop
    // ------------------------------------------------------------------
    int audioIdx = 0;
    for (const auto& track : toc.tracks) {
        if (!track.isAudio) continue;
        if (m_cancelled) break;

        const bool isFirst = (audioIdx == 0);
        const bool isLast  = (audioIdx == totalAudio - 1);
        (void)isFirst; (void)isLast;  // used by AccurateRip later

        if (m_cb.onTrackStart)
            m_cb.onTrackStart(track.number, totalAudio, track.sectorCount);

        auto progressCb = [this](const rip::RipProgress& p) {
            if (m_cb.onTrackProgress) m_cb.onTrackProgress(p);
        };

        auto result = engine.ripTrack(track, track.number, progressCb);

        if (result.cancelled || m_cancelled) {
            allTrackPcm.push_back({});
            writtenFiles.push_back({});
            ++audioIdx;
            break;
        }

        int suspectSectors = 0, c2Sectors = 0;
        for (const auto& sr : result.sectors) {
            if (sr.confidence == 0) ++suspectSectors;
            if (sr.hasC2Errors)     ++c2Sectors;
        }

        std::filesystem::path filePath;
        bool writeOk = false;

        if (result.ok) {
            // Determine filename
            std::string title;
            if (release) {
                int mbIdx = track.number - 1;
                if (mbIdx >= 0 && mbIdx < static_cast<int>(release->tracks.size()))
                    title = release->tracks[static_cast<size_t>(mbIdx)].title;
            }
            std::string filename = makeTrackName(track.number, title, ext);
            filePath = m_config.outputDir / filename;

            if (isFlac) {
                encode::EncoderSettings es = m_config.encoderSettings;
                es.format        = encode::Format::FLAC;
                es.totalSamples  = static_cast<uint64_t>(result.data.size()) / 4u;

                encode::FlacEncoder encoder;
                // Set minimal track number tag (full tags written later by TagWriter)
                encoder.setTag("TRACKNUMBER", std::to_string(track.number));
                encoder.setTag("TRACKTOTAL",  std::to_string(totalAudio));
                if (coverArt.ok)
                    encoder.setPicture(coverArt.data, coverArt.mimeType);

                std::string encErr;
                if (encoder.open(filePath, es)) {
                    std::vector<int32_t> samples;
                    encode::cdBytesToSamples(result.data.data(), result.data.size(), samples);
                    writeOk = encoder.writeSamples(samples) && encoder.finalize();
                    if (!writeOk) encErr = encoder.lastError();
                } else {
                    encErr = encoder.lastError();
                }
                if (!writeOk && m_cb.onError)
                    m_cb.onError("Encode failed for track " + std::to_string(track.number) + ": " + encErr);
            } else {
                // WAV
                encode::EncoderSettings es = m_config.encoderSettings;
                es.format       = encode::Format::WAV;
                es.totalSamples = static_cast<uint64_t>(result.data.size()) / 4u;

                encode::WavEncoder wavEnc;
                std::string wavErr;
                if (wavEnc.open(filePath, es)) {
                    std::vector<int32_t> samples;
                    encode::cdBytesToSamples(result.data.data(), result.data.size(), samples);
                    writeOk = wavEnc.writeSamples(samples) && wavEnc.finalize();
                    if (!writeOk) wavErr = wavEnc.lastError();
                } else {
                    wavErr = wavEnc.lastError();
                }
                if (!writeOk && m_cb.onError)
                    m_cb.onError("WAV encode failed for track " +
                                 std::to_string(track.number) + ": " + wavErr);
            }
        }

        TrackDoneInfo info;
        info.trackNumber    = track.number;
        info.crc32          = result.crc32;
        info.suspectSectors = suspectSectors;
        info.c2Sectors      = c2Sectors;
        info.outputPath     = writeOk ? filePath : std::filesystem::path{};
        info.ok             = result.ok && writeOk;
        if (m_cb.onTrackDone) m_cb.onTrackDone(info);

        allTrackPcm.push_back(writeOk ? std::move(result.data) : std::vector<uint8_t>{});
        writtenFiles.push_back(writeOk ? filePath : std::filesystem::path{});
        ++audioIdx;
    }

    engine.close();

    if (m_cancelled) {
        setState(PipelineState::Cancelled);
        if (m_cb.onCancelled) m_cb.onCancelled();
        return;
    }

    // ------------------------------------------------------------------
    // 5b. Single-file FLAC encode (optional; FLAC format only)
    //     All ripped tracks are concatenated into one .flac with an
    //     embedded CUESHEET block.  writtenFiles[i] all point to this
    //     one file so that tagging and cue-sheet steps work unchanged.
    // ------------------------------------------------------------------
    std::filesystem::path    singleFilePath;
    std::vector<uint64_t>    singleFileTrackOffsets;  // stereo frames; used by cue sheet step

    if (m_config.singleFile && isFlac && !allTrackPcm.empty()) {
        // Build album filename from MB metadata (or "disc.flac")
        std::string albumName = "disc";
        if (release) {
            std::string art = release->artist.empty() ? "" : sanitise(release->artist);
            std::string ttl = release->title.empty()  ? "" : sanitise(release->title);
            if (!art.empty() || !ttl.empty())
                albumName = (art.empty() ? ttl : (ttl.empty() ? art : art + " - " + ttl));
        }
        singleFilePath = m_config.outputDir / (albumName + ".flac");

        // Compute per-track stereo-frame offsets.
        // CD-DA: 4 bytes per stereo frame (2 bytes L + 2 bytes R).
        // FLAC encoder, CUESHEET block, and cue sheet all use stereo frames.
        // lbaToMsf() divides by 588 (44100 / 75 frames/sec) — same unit.
        singleFileTrackOffsets.reserve(allTrackPcm.size());
        uint64_t accumFrames = 0;
        for (const auto& pcm : allTrackPcm) {
            singleFileTrackOffsets.push_back(accumFrames);
            accumFrames += pcm.size() / 4u;  // bytes / 4 = stereo frames
        }
        const uint64_t totalFrames = accumFrames;

        encode::EncoderSettings es = m_config.encoderSettings;
        es.format       = encode::Format::FLAC;
        es.totalSamples = totalFrames;

        encode::FlacEncoder sfEnc;

        // Set album-level tags
        if (release) {
            sfEnc.setTag("ARTIST",     release->artist);
            sfEnc.setTag("ALBUM",      release->title);
            sfEnc.setTag("DATE",       release->date.substr(0, 4));
            sfEnc.setTag("TRACKTOTAL", std::to_string(totalAudio));
        }
        if (coverArt.ok)
            sfEnc.setPicture(coverArt.data, coverArt.mimeType);

        // Add CUESHEET tracks
        {
            int ai = 0;
            for (const auto& track : toc.tracks) {
                if (!track.isAudio) continue;
                encode::FlacEncoder::CueSheetTrack ct;
                ct.number       = track.number;
                ct.sampleOffset = singleFileTrackOffsets[static_cast<size_t>(ai)];
                ct.isAudio      = true;
                sfEnc.addCueSheetTrack(ct);
                ++ai;
            }
            sfEnc.setCueSheetLeadOut(totalFrames);
        }

        std::string sfErr;
        bool sfOk = false;
        if (sfEnc.open(singleFilePath, es)) {
            sfOk = true;
            for (const auto& pcm : allTrackPcm) {
                if (pcm.empty()) continue;
                std::vector<int32_t> samples;
                encode::cdBytesToSamples(pcm.data(), pcm.size(), samples);
                if (!sfEnc.writeSamples(samples)) { sfOk = false; break; }
            }
            if (sfOk) sfOk = sfEnc.finalize();
            if (!sfOk) sfErr = sfEnc.lastError();
        } else {
            sfErr = sfEnc.lastError();
        }

        if (!sfOk && m_cb.onError)
            m_cb.onError("Single-file encode failed: " + sfErr);

        if (sfOk) {
            // Point all writtenFiles entries to the single output file so that
            // the tagging and cue sheet steps work unchanged.
            for (auto& p : writtenFiles)
                p = singleFilePath;
        }
    }

    // ------------------------------------------------------------------
    // 6.  AccurateRip verification
    // ------------------------------------------------------------------
    if (m_config.verifyAccurateRip && static_cast<int>(allTrackPcm.size()) == totalAudio) {
        setState(PipelineState::Verifying);
        auto arResult = verify::AccurateRip::verify(toc, allTrackPcm);
        if (m_cb.onVerifyDone) m_cb.onVerifyDone(arResult);

        // ------------------------------------------------------------------
        // 6b. Drive offset auto-detection (opt-in)
        //     Triggered when: enabled AND no tracks matched at offset=0
        // ------------------------------------------------------------------
        if (m_config.autoDetectOffset && !m_cancelled) {
            const bool anyMatched = [&] {
                for (const auto& t : arResult.tracks)
                    if (t.matched) return true;
                return false;
            }();

            if (!anyMatched && arResult.lookupOk) {
                auto offsetResult = verify::AccurateRip::detectOffset(toc, allTrackPcm);
                if (m_cb.onOffsetDetected) m_cb.onOffsetDetected(offsetResult);
            }
        }
    }

    // ------------------------------------------------------------------
    // 7.  Tagging (FLAC only, requires a selected release)
    // ------------------------------------------------------------------
    if (m_config.writeTags && isFlac && release) {
        setState(PipelineState::Tagging);
        int tagged   = 0;
        int audioIdx2 = 0;  // counts audio tracks only — matches writtenFiles index

        for (int i = 0; i < static_cast<int>(toc.tracks.size()); ++i) {
            const auto& track = toc.tracks[static_cast<size_t>(i)];
            if (!track.isAudio) continue;

            const int ai = audioIdx2++;
            if (ai >= static_cast<int>(writtenFiles.size())) continue;

            const auto& filePath = writtenFiles[static_cast<size_t>(ai)];
            if (filePath.empty()) continue;

            // In single-file mode the encoder already set album-level tags;
            // skip per-track tagging and renaming to avoid clobbering them.
            if (m_config.singleFile) {
                ++tagged;
                continue;
            }

            // MB track list is 0-based audio tracks; use ai as the index
            int mbIdx = ai;
            if (mbIdx >= static_cast<int>(release->tracks.size()))
                mbIdx = static_cast<int>(release->tracks.size()) - 1;

            auto tags = metadata::TrackTags::from(*release, mbIdx);

            std::string tagErr;
            if (!metadata::TagWriter::writeFlac(filePath, tags, &tagErr)) {
                if (m_cb.onError)
                    m_cb.onError("Tag error for track " + std::to_string(track.number) + ": " + tagErr);
                continue;
            }

            // Rename to descriptive filename using final MB title
            const std::string newName = makeTrackName(track.number, tags.title, "flac");
            std::filesystem::path newPath = filePath.parent_path() / newName;
            if (newPath != filePath) {
                std::error_code renameEc;
                std::filesystem::rename(filePath, newPath, renameEc);
                // Update in-place (ai is the audio-track index matching writtenFiles)
                writtenFiles[static_cast<size_t>(ai)] = renameEc ? filePath : newPath;
            }

            ++tagged;
        }

        if (m_cb.onTagsDone) m_cb.onTagsDone(tagged);
    }

    // ------------------------------------------------------------------
    // 8.  Cue sheet (optional)
    // ------------------------------------------------------------------
    if (m_config.writeCueSheet) {
        const std::string discId = metadata::DiscId::calculate(toc);

        std::string cueContent;
        if (m_config.singleFile && !singleFilePath.empty()) {
            // Single-file mode: one FILE line, per-track INDEX offsets
            std::vector<std::filesystem::path> sfPaths = { singleFilePath };
            cueContent = metadata::CueSheet::generate(
                toc, sfPaths, release, discId, singleFileTrackOffsets);
        } else {
            cueContent = metadata::CueSheet::generate(
                toc, writtenFiles, release, discId);
        }

        const std::string cueName  = metadata::CueSheet::filename(release);
        const std::filesystem::path cuePath = m_config.outputDir / cueName;

        std::string cueErr;
        if (!metadata::CueSheet::write(cuePath, cueContent, &cueErr) && m_cb.onError)
            m_cb.onError("Cue sheet: " + cueErr);  // non-fatal
    }

    // ------------------------------------------------------------------
    // 9.  Eject (optional)
    // ------------------------------------------------------------------
    if (m_config.ejectWhenDone) {
        drive::Drive drv(drivePath, drivePath);
        drv.eject();   // best-effort; ignore failure
    }

    // ------------------------------------------------------------------
    // 10. Done
    // ------------------------------------------------------------------
    setState(PipelineState::Complete);
    if (m_cb.onComplete) m_cb.onComplete();
}

} // namespace atomicripper::pipeline
