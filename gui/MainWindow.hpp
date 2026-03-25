#pragma once
#include <core/drive/Drive.hpp>
#include <core/drive/DriveEnumerator.hpp>
#include <core/metadata/MusicBrainz.hpp>
#include <core/pipeline/Pipeline.hpp>
#include <core/verify/AccurateRip.hpp>

#include <memory>
#include <vector>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace atomicripper::gui {

class TrackTableWidget;

// ---------------------------------------------------------------------------
// MainWindow — top-level application window
//
// Layout (vertical):
//   Header  — drive selector + disc summary
//   Center  — QStackedWidget:  page 0 = config,  page 1 = rip progress
//   Footer  — output path + Start / Cancel buttons
// ---------------------------------------------------------------------------
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* ev) override;

private slots:
    // Drive / disc
    void refreshDrives();
    void onDriveChanged(int index);
    void pollDiscStatus();

    // Rip control
    void startRip();
    void cancelRip();
    void browseOutputDir();
    void onFormatChanged(int index);

    // Pipeline callbacks — all invoked on the main thread
    void onStateChangedUI(int state);
    void onTocReadUI(drive::TOC toc);
    void showReleaseDialog(metadata::MbResult mb);
    void onTrackStartUI(int trackNumber, int total, uint32_t sectors);
    void onTrackProgressUI(rip::RipProgress p);
    void onTrackDoneUI(pipeline::TrackDoneInfo info);
    void onVerifyDoneUI(verify::ArDiscResult ar);
    void onOffsetDetectedUI(verify::ArOffsetResult r);
    void onTagsDoneUI(int count);
    void onCompleteUI();
    void onErrorUI(QString msg);
    void onCancelledUI();

private:
    // --- Build helpers ---
    void buildHeader(QWidget* parent, QVBoxLayout* vbox);
    void buildConfigPage(QWidget* page);
    void buildProgressPage(QWidget* page);
    void buildFooter(QWidget* parent, QVBoxLayout* vbox);

    void updateStartButton();
    void updateFlacOnlyWidgets();
    pipeline::PipelineConfig buildConfig() const;

    // --- Drive helpers ---
    std::string selectedDrivePath() const;

    // --- Widgets ---
    // Header
    QComboBox*    m_driveCombo    = nullptr;
    QPushButton*  m_refreshBtn    = nullptr;
    QLabel*       m_discInfoLabel = nullptr;

    // Stacked center
    QStackedWidget* m_stack       = nullptr;

    // Config page widgets
    QLineEdit*    m_outputDir     = nullptr;
    QComboBox*    m_formatCombo   = nullptr;
    QComboBox*    m_modeCombo     = nullptr;
    QSpinBox*     m_retriesSpin   = nullptr;
    QSpinBox*     m_offsetSpin    = nullptr;
    QCheckBox*    m_chkMetadata   = nullptr;
    QCheckBox*    m_chkAutoSelect = nullptr;
    QCheckBox*    m_chkCoverArt   = nullptr;
    QCheckBox*    m_chkWriteTags  = nullptr;
    QCheckBox*    m_chkAccurateRip= nullptr;
    QCheckBox*    m_chkDetectOff  = nullptr;
    QCheckBox*    m_chkCueSheet   = nullptr;
    QCheckBox*    m_chkSingleFile = nullptr;
    QCheckBox*    m_chkEject      = nullptr;

    // Progress page widgets
    QLabel*           m_stateLabel    = nullptr;
    QProgressBar*     m_trackProgress = nullptr;
    QLabel*           m_speedLabel    = nullptr;
    TrackTableWidget* m_trackTable    = nullptr;
    QLabel*           m_errorLabel    = nullptr;
    QLabel*           m_offsetResult  = nullptr;

    // Footer
    QPushButton*  m_startBtn     = nullptr;
    QPushButton*  m_cancelBtn    = nullptr;

    // --- State ---
    std::vector<drive::Drive>            m_drives;
    drive::TOC                           m_currentToc;
    bool                                 m_hasToc = false;
    std::unique_ptr<pipeline::Pipeline>  m_pipeline;
    QTimer*                              m_discPollTimer = nullptr;
};

} // namespace atomicripper::gui
