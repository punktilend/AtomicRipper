#include "MainWindow.hpp"
#include "ReleaseDialog.hpp"
#include "TrackTableWidget.hpp"

#include <core/metadata/DiscId.hpp>

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QMetaObject>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

namespace atomicripper::gui {

// ===========================================================================
// Construction
// ===========================================================================

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("AtomicRipper");
    setMinimumSize(800, 560);

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* vbox = new QVBoxLayout(central);
    vbox->setSpacing(8);
    vbox->setContentsMargins(10, 10, 10, 10);

    buildHeader(central, vbox);

    // Separator
    auto* line = new QFrame(central);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(line);

    // Center: stacked widget
    m_stack = new QStackedWidget(central);

    auto* configPage = new QWidget(m_stack);
    buildConfigPage(configPage);
    m_stack->addWidget(configPage);       // page 0 = config

    auto* progressPage = new QWidget(m_stack);
    buildProgressPage(progressPage);
    m_stack->addWidget(progressPage);     // page 1 = progress

    vbox->addWidget(m_stack, /*stretch=*/1);

    auto* line2 = new QFrame(central);
    line2->setFrameShape(QFrame::HLine);
    line2->setFrameShadow(QFrame::Sunken);
    vbox->addWidget(line2);

    buildFooter(central, vbox);

    // Disc poll timer — runs only when idle
    m_discPollTimer = new QTimer(this);
    m_discPollTimer->setInterval(2000);
    connect(m_discPollTimer, &QTimer::timeout, this, &MainWindow::pollDiscStatus);
    m_discPollTimer->start();

    // Populate drives on startup
    refreshDrives();
}

MainWindow::~MainWindow() {
    if (m_pipeline) {
        m_pipeline->cancel();
        m_pipeline.reset();   // joins the worker thread
    }
}

// ===========================================================================
// Layout builders
// ===========================================================================

void MainWindow::buildHeader(QWidget* /*parent*/, QVBoxLayout* vbox) {
    auto* row = new QHBoxLayout();
    row->setSpacing(6);

    row->addWidget(new QLabel("Drive:", this));

    m_driveCombo = new QComboBox(this);
    m_driveCombo->setMinimumWidth(220);
    row->addWidget(m_driveCombo);

    m_refreshBtn = new QPushButton("⟳", this);
    m_refreshBtn->setFixedWidth(28);
    m_refreshBtn->setToolTip("Refresh drive list");
    row->addWidget(m_refreshBtn);

    row->addSpacing(16);

    m_discInfoLabel = new QLabel("No disc", this);
    m_discInfoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row->addWidget(m_discInfoLabel);

    vbox->addLayout(row);

    connect(m_refreshBtn,  &QPushButton::clicked,
            this,          &MainWindow::refreshDrives);
    connect(m_driveCombo,  qOverload<int>(&QComboBox::currentIndexChanged),
            this,          &MainWindow::onDriveChanged);
}

void MainWindow::buildConfigPage(QWidget* page) {
    auto* hbox = new QHBoxLayout(page);
    hbox->setSpacing(12);

    // ---- Left: rip settings ------------------------------------------------
    auto* ripBox = new QGroupBox("Rip Settings", page);
    auto* ripForm = new QVBoxLayout(ripBox);
    ripForm->setSpacing(6);

    // Output folder
    auto* dirRow = new QHBoxLayout();
    m_outputDir = new QLineEdit(ripBox);
    m_outputDir->setPlaceholderText("Output folder…");
    auto* browseBtn = new QPushButton("Browse…", ripBox);
    browseBtn->setFixedWidth(70);
    dirRow->addWidget(m_outputDir);
    dirRow->addWidget(browseBtn);
    ripForm->addLayout(dirRow);

    // Format
    auto* fmtRow = new QHBoxLayout();
    fmtRow->addWidget(new QLabel("Format:", ripBox));
    m_formatCombo = new QComboBox(ripBox);
    m_formatCombo->addItem("FLAC");
    m_formatCombo->addItem("WAV");
    fmtRow->addWidget(m_formatCombo);
    fmtRow->addStretch();
    ripForm->addLayout(fmtRow);

    // Rip mode
    auto* modeRow = new QHBoxLayout();
    modeRow->addWidget(new QLabel("Mode:", ripBox));
    m_modeCombo = new QComboBox(ripBox);
    m_modeCombo->addItem("Secure");
    m_modeCombo->addItem("Burst");
    m_modeCombo->addItem("Paranoia");
    modeRow->addWidget(m_modeCombo);
    modeRow->addStretch();
    ripForm->addLayout(modeRow);

    // Max retries
    auto* retryRow = new QHBoxLayout();
    retryRow->addWidget(new QLabel("Max retries:", ripBox));
    m_retriesSpin = new QSpinBox(ripBox);
    m_retriesSpin->setRange(1, 64);
    m_retriesSpin->setValue(16);
    retryRow->addWidget(m_retriesSpin);
    retryRow->addStretch();
    ripForm->addLayout(retryRow);

    // Drive offset
    auto* offRow = new QHBoxLayout();
    offRow->addWidget(new QLabel("Drive offset (samples):", ripBox));
    m_offsetSpin = new QSpinBox(ripBox);
    m_offsetSpin->setRange(-2048, 2048);
    m_offsetSpin->setValue(0);
    m_offsetSpin->setToolTip("Read offset correction in samples. "
                              "Use --detect-offset to find this automatically.");
    offRow->addWidget(m_offsetSpin);
    offRow->addStretch();
    ripForm->addLayout(offRow);

    ripForm->addStretch();
    hbox->addWidget(ripBox, /*stretch=*/1);

    // ---- Right: options ----------------------------------------------------
    auto* optBox = new QGroupBox("Options", page);
    auto* optLayout = new QVBoxLayout(optBox);
    optLayout->setSpacing(4);

    auto makeChk = [&](const QString& label, bool checked = true) {
        auto* cb = new QCheckBox(label, optBox);
        cb->setChecked(checked);
        optLayout->addWidget(cb);
        return cb;
    };

    m_chkMetadata    = makeChk("Fetch MusicBrainz metadata");
    m_chkAutoSelect  = makeChk("Auto-select first release", false);
    m_chkCoverArt    = makeChk("Embed cover art  (FLAC)");
    m_chkWriteTags   = makeChk("Write tags  (FLAC)");
    m_chkAccurateRip = makeChk("Verify AccurateRip");
    m_chkDetectOff   = makeChk("Auto-detect drive offset", false);
    m_chkCueSheet    = makeChk("Write cue sheet");
    m_chkSingleFile  = makeChk("Single-file FLAC  (FLAC)", false);
    m_chkEject       = makeChk("Eject disc when done", false);

    optLayout->addStretch();
    hbox->addWidget(optBox);

    // Connections
    connect(browseBtn,     &QPushButton::clicked,
            this,          &MainWindow::browseOutputDir);
    connect(m_outputDir,   &QLineEdit::textChanged,
            this,          [this](const QString&) { updateStartButton(); });
    connect(m_formatCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this,          &MainWindow::onFormatChanged);
    connect(m_chkMetadata, &QCheckBox::toggled,
            this,          [this](bool on) {
                m_chkAutoSelect->setEnabled(on);
                m_chkCoverArt->setEnabled(on && m_formatCombo->currentIndex() == 0);
                m_chkWriteTags->setEnabled(on && m_formatCombo->currentIndex() == 0);
            });
}

void MainWindow::buildProgressPage(QWidget* page) {
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(6);

    m_stateLabel = new QLabel("Idle", page);
    QFont stateFont = m_stateLabel->font();
    stateFont.setBold(true);
    stateFont.setPointSize(stateFont.pointSize() + 1);
    m_stateLabel->setFont(stateFont);
    vbox->addWidget(m_stateLabel);

    m_trackProgress = new QProgressBar(page);
    m_trackProgress->setRange(0, 100);
    m_trackProgress->setValue(0);
    m_trackProgress->setVisible(false);
    vbox->addWidget(m_trackProgress);

    m_speedLabel = new QLabel(page);
    m_speedLabel->setVisible(false);
    vbox->addWidget(m_speedLabel);

    m_trackTable = new TrackTableWidget(page);
    vbox->addWidget(m_trackTable, /*stretch=*/1);

    m_offsetResult = new QLabel(page);
    m_offsetResult->setVisible(false);
    vbox->addWidget(m_offsetResult);

    m_errorLabel = new QLabel(page);
    m_errorLabel->setStyleSheet("color: #F44336; font-weight: bold;");
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setVisible(false);
    vbox->addWidget(m_errorLabel);
}

void MainWindow::buildFooter(QWidget* /*parent*/, QVBoxLayout* vbox) {
    auto* row = new QHBoxLayout();
    row->setSpacing(8);

    row->addStretch();

    m_cancelBtn = new QPushButton("Cancel", this);
    m_cancelBtn->setVisible(false);
    row->addWidget(m_cancelBtn);

    m_startBtn = new QPushButton("Start Rip", this);
    m_startBtn->setDefault(true);
    m_startBtn->setFixedWidth(100);
    m_startBtn->setEnabled(false);
    row->addWidget(m_startBtn);

    vbox->addLayout(row);

    connect(m_startBtn,  &QPushButton::clicked, this, &MainWindow::startRip);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MainWindow::cancelRip);
}

// ===========================================================================
// Drive management
// ===========================================================================

void MainWindow::refreshDrives() {
    const QString current = m_driveCombo->currentText();
    m_driveCombo->blockSignals(true);
    m_driveCombo->clear();
    m_drives = drive::DriveEnumerator::enumerate();

    for (const auto& drv : m_drives) {
        m_driveCombo->addItem(
            QString::fromStdString(drv.description()));
    }

    // Restore previous selection if possible
    int idx = m_driveCombo->findText(current);
    m_driveCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_driveCombo->blockSignals(false);

    onDriveChanged(m_driveCombo->currentIndex());
}

void MainWindow::onDriveChanged(int index) {
    m_hasToc = false;
    m_currentToc = {};
    m_discInfoLabel->setText("Checking…");
    updateStartButton();

    if (index < 0 || index >= static_cast<int>(m_drives.size())) {
        m_discInfoLabel->setText("No drive selected");
        return;
    }

    const auto& drv = m_drives[static_cast<size_t>(index)];
    const auto status = drv.status();

    if (status != drive::DriveStatus::Ready) {
        m_discInfoLabel->setText(
            status == drive::DriveStatus::Empty ? "No disc" : "Drive not ready");
        updateStartButton();
        return;
    }

    auto tocOpt = drv.readTOC();
    if (!tocOpt || !tocOpt->isValid()) {
        m_discInfoLabel->setText("Could not read TOC");
        updateStartButton();
        return;
    }

    m_currentToc = *tocOpt;
    m_hasToc = true;

    const int tracks = m_currentToc.audioTrackCount();
    const double secs = m_currentToc.durationSeconds();
    const int m = static_cast<int>(secs) / 60;
    const int s = static_cast<int>(secs) % 60;
    m_discInfoLabel->setText(
        QString("%1 audio track%2  ·  %3:%4")
            .arg(tracks)
            .arg(tracks == 1 ? "" : "s")
            .arg(m)
            .arg(s, 2, 10, QChar('0')));

    updateStartButton();
}

void MainWindow::pollDiscStatus() {
    // Only poll when idle
    if (m_pipeline && m_pipeline->isRunning()) return;

    const int idx = m_driveCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_drives.size())) return;

    const auto status = m_drives[static_cast<size_t>(idx)].status();
    const bool wasReady = m_hasToc;
    const bool nowReady = (status == drive::DriveStatus::Ready);

    if (wasReady != nowReady)
        onDriveChanged(idx);   // refresh disc info
}

// ===========================================================================
// Configuration helpers
// ===========================================================================

std::string MainWindow::selectedDrivePath() const {
    const int idx = m_driveCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_drives.size())) return {};
    return m_drives[static_cast<size_t>(idx)].path();
}

pipeline::PipelineConfig MainWindow::buildConfig() const {
    pipeline::PipelineConfig cfg;

    cfg.outputDir  = m_outputDir->text().toStdString();
    cfg.format     = (m_formatCombo->currentIndex() == 0)
                     ? encode::Format::FLAC : encode::Format::WAV;

    const bool isFlac = (cfg.format == encode::Format::FLAC);

    rip::RipMode mode = rip::RipMode::Secure;
    if (m_modeCombo->currentIndex() == 1) mode = rip::RipMode::Burst;
    if (m_modeCombo->currentIndex() == 2) mode = rip::RipMode::Paranoia;

    cfg.ripSettings.mode         = mode;
    cfg.ripSettings.maxRetries   = m_retriesSpin->value();
    cfg.ripSettings.minMatches   = 2;
    cfg.ripSettings.useC2Errors  = true;
    cfg.ripSettings.driveOffset  = m_offsetSpin->value();

    cfg.encoderSettings.compressionLevel = 8;

    cfg.fetchMetadata     = m_chkMetadata->isChecked();
    cfg.autoSelectRelease = m_chkAutoSelect->isChecked();
    cfg.embedCoverArt     = isFlac && m_chkCoverArt->isChecked();
    cfg.writeTags         = isFlac && m_chkWriteTags->isChecked();
    cfg.verifyAccurateRip = m_chkAccurateRip->isChecked();
    cfg.autoDetectOffset  = m_chkDetectOff->isChecked();
    cfg.writeCueSheet     = m_chkCueSheet->isChecked();
    cfg.singleFile        = isFlac && m_chkSingleFile->isChecked();
    cfg.ejectWhenDone     = m_chkEject->isChecked();

    return cfg;
}

void MainWindow::updateStartButton() {
    const bool canStart =
        m_hasToc &&
        !m_outputDir->text().trimmed().isEmpty() &&
        !selectedDrivePath().empty() &&
        !(m_pipeline && m_pipeline->isRunning());
    m_startBtn->setEnabled(canStart);
}

void MainWindow::updateFlacOnlyWidgets() {
    const bool isFlac = (m_formatCombo->currentIndex() == 0);
    m_chkCoverArt->setEnabled(isFlac && m_chkMetadata->isChecked());
    m_chkWriteTags->setEnabled(isFlac && m_chkMetadata->isChecked());
    m_chkSingleFile->setEnabled(isFlac);
}

// ===========================================================================
// UI event slots
// ===========================================================================

void MainWindow::browseOutputDir() {
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Output Folder",
        m_outputDir->text().isEmpty()
            ? QDir::homePath() : m_outputDir->text());
    if (!dir.isEmpty())
        m_outputDir->setText(dir);
}

void MainWindow::onFormatChanged(int /*index*/) {
    updateFlacOnlyWidgets();
    updateStartButton();
}

// ===========================================================================
// Rip control
// ===========================================================================

void MainWindow::startRip() {
    const std::string drivePath = selectedDrivePath();
    if (drivePath.empty() || !m_hasToc) return;

    const QString outDir = m_outputDir->text().trimmed();
    if (outDir.isEmpty()) {
        QMessageBox::warning(this, "AtomicRipper",
                             "Please select an output folder.");
        return;
    }

    // Reset progress UI
    m_trackTable->reset();
    m_errorLabel->setVisible(false);
    m_offsetResult->setVisible(false);
    m_speedLabel->setVisible(false);
    m_trackProgress->setValue(0);
    m_trackProgress->setVisible(false);
    m_stateLabel->setText("Starting…");

    m_startBtn->setEnabled(false);
    m_cancelBtn->setVisible(true);
    m_stack->setCurrentIndex(1);
    m_discPollTimer->stop();

    // Build config and callbacks
    pipeline::PipelineConfig cfg = buildConfig();
    pipeline::PipelineCallbacks cb;

    // NOTE: All callbacks fire from the worker thread.
    // We use QMetaObject::invokeMethod with QueuedConnection for most,
    // and BlockingQueuedConnection for onMetadataReady (needs synchronous
    // selectRelease() before returning).

    cb.onStateChanged = [this](pipeline::PipelineState s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            onStateChangedUI(static_cast<int>(s));
        }, Qt::QueuedConnection);
    };

    cb.onTocRead = [this](const drive::TOC& toc) {
        drive::TOC copy = toc;
        QMetaObject::invokeMethod(this, [this, copy]() {
            onTocReadUI(copy);
        }, Qt::QueuedConnection);
    };

    cb.onMetadataReady = [this](const metadata::MbResult& mb) {
        metadata::MbResult copy = mb;
        // BlockingQueuedConnection: worker blocks until showReleaseDialog returns.
        // showReleaseDialog MUST call pipeline->selectRelease() or cancel().
        QMetaObject::invokeMethod(this, [this, copy]() {
            showReleaseDialog(copy);
        }, Qt::BlockingQueuedConnection);
    };

    cb.onTrackStart = [this](int num, int total, uint32_t sectors) {
        QMetaObject::invokeMethod(this, [this, num, total, sectors]() {
            onTrackStartUI(num, total, sectors);
        }, Qt::QueuedConnection);
    };

    cb.onTrackProgress = [this](const rip::RipProgress& p) {
        rip::RipProgress copy = p;
        QMetaObject::invokeMethod(this, [this, copy]() {
            onTrackProgressUI(copy);
        }, Qt::QueuedConnection);
    };

    cb.onTrackDone = [this](const pipeline::TrackDoneInfo& info) {
        pipeline::TrackDoneInfo copy = info;
        QMetaObject::invokeMethod(this, [this, copy]() {
            onTrackDoneUI(copy);
        }, Qt::QueuedConnection);
    };

    cb.onVerifyDone = [this](const verify::ArDiscResult& ar) {
        verify::ArDiscResult copy = ar;
        QMetaObject::invokeMethod(this, [this, copy]() {
            onVerifyDoneUI(copy);
        }, Qt::QueuedConnection);
    };

    cb.onOffsetDetected = [this](const verify::ArOffsetResult& r) {
        verify::ArOffsetResult copy = r;
        QMetaObject::invokeMethod(this, [this, copy]() {
            onOffsetDetectedUI(copy);
        }, Qt::QueuedConnection);
    };

    cb.onTagsDone = [this](int count) {
        QMetaObject::invokeMethod(this, [this, count]() {
            onTagsDoneUI(count);
        }, Qt::QueuedConnection);
    };

    cb.onComplete = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            onCompleteUI();
        }, Qt::QueuedConnection);
    };

    cb.onError = [this](const std::string& msg) {
        QString qmsg = QString::fromStdString(msg);
        QMetaObject::invokeMethod(this, [this, qmsg]() {
            onErrorUI(qmsg);
        }, Qt::QueuedConnection);
    };

    cb.onCancelled = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            onCancelledUI();
        }, Qt::QueuedConnection);
    };

    // Pre-populate track table from the known TOC (title column filled after MB)
    m_trackTable->populateFromToc(m_currentToc);

    m_pipeline = std::make_unique<pipeline::Pipeline>(std::move(cfg), std::move(cb));
    m_pipeline->start(drivePath);
}

void MainWindow::cancelRip() {
    if (m_pipeline)
        m_pipeline->cancel();
    m_cancelBtn->setEnabled(false);
}

// ===========================================================================
// Pipeline callback handlers (main thread)
// ===========================================================================

static QString stateString(pipeline::PipelineState s) {
    using S = pipeline::PipelineState;
    switch (s) {
    case S::Idle:              return "Idle";
    case S::ReadingToc:        return "Reading TOC…";
    case S::FetchingMetadata:  return "Fetching MusicBrainz metadata…";
    case S::WaitingForRelease: return "Waiting for release selection…";
    case S::Ripping:           return "Ripping…";
    case S::Verifying:         return "Verifying (AccurateRip)…";
    case S::Tagging:           return "Writing tags…";
    case S::Complete:          return "Done!";
    case S::Cancelled:         return "Cancelled";
    case S::Error:             return "Error";
    }
    return {};
}

void MainWindow::onStateChangedUI(int state) {
    const auto s = static_cast<pipeline::PipelineState>(state);
    m_stateLabel->setText(stateString(s));

    const bool terminal = (s == pipeline::PipelineState::Complete ||
                           s == pipeline::PipelineState::Cancelled ||
                           s == pipeline::PipelineState::Error);
    if (terminal) {
        m_cancelBtn->setVisible(false);
        m_trackProgress->setVisible(false);
        m_speedLabel->setVisible(false);

        // Defer pipeline reset by one event loop cycle to let the worker finish
        QTimer::singleShot(100, this, [this]() {
            m_pipeline.reset();
            m_discPollTimer->start();
            onDriveChanged(m_driveCombo->currentIndex());
            updateStartButton();
        });
    }

    const bool ripping = (s == pipeline::PipelineState::Ripping);
    m_trackProgress->setVisible(ripping);
    m_speedLabel->setVisible(ripping);
}

void MainWindow::onTocReadUI(drive::TOC toc) {
    m_currentToc = toc;
    m_hasToc     = true;
    // Track table already populated in startRip(); this is a no-op here.
    (void)toc;
}

void MainWindow::showReleaseDialog(metadata::MbResult mb) {
    // Called via BlockingQueuedConnection — must call selectRelease() before returning.
    if (mb.releases.empty() || !m_pipeline) return;

    if (mb.releases.size() == 1) {
        // Only one choice — auto-select
        m_trackTable->populateFromToc(m_currentToc, &mb.releases[0]);
        m_pipeline->selectRelease(0);
        return;
    }

    ReleaseDialog dlg(mb, this);
    const int accepted = dlg.exec();
    const int idx      = (accepted == QDialog::Accepted) ? dlg.selectedIndex() : -1;

    if (idx >= 0 && idx < static_cast<int>(mb.releases.size())) {
        m_trackTable->populateFromToc(m_currentToc, &mb.releases[static_cast<size_t>(idx)]);
        if (m_pipeline) m_pipeline->selectRelease(idx);
    } else {
        if (m_pipeline) m_pipeline->cancel();
    }
}

void MainWindow::onTrackStartUI(int trackNumber, int total, uint32_t sectors) {
    m_stateLabel->setText(QString("Ripping track %1 / %2  (%3 sectors)")
        .arg(trackNumber).arg(total).arg(sectors));
    m_trackProgress->setValue(0);
    m_trackTable->setActiveTrack(trackNumber);
}

void MainWindow::onTrackProgressUI(rip::RipProgress p) {
    if (p.totalSectors > 0) {
        const int pct = static_cast<int>(
            100.0f * static_cast<float>(p.currentSector) / p.totalSectors);
        m_trackProgress->setValue(pct);
    }
    m_speedLabel->setText(
        QString("%1×  ·  %2 retr%3")
            .arg(static_cast<double>(p.speedX), 0, 'f', 1)
            .arg(p.totalRetries)
            .arg(p.totalRetries == 1 ? "y" : "ies"));
}

void MainWindow::onTrackDoneUI(pipeline::TrackDoneInfo info) {
    m_trackTable->updateTrackDone(info);
}

void MainWindow::onVerifyDoneUI(verify::ArDiscResult ar) {
    m_stateLabel->setText("Verifying (AccurateRip)…");
    m_trackTable->updateArResults(ar);
}

void MainWindow::onOffsetDetectedUI(verify::ArOffsetResult r) {
    if (r.found) {
        m_offsetResult->setText(
            QString("Detected drive offset: <b>%1%2</b> samples  "
                    "(conf=%3, %4 track%5 matched)  —  "
                    "re-rip with offset %1%2")
                .arg(r.sampleOffset >= 0 ? "+" : "")
                .arg(r.sampleOffset)
                .arg(r.confidence)
                .arg(r.tracksMatched)
                .arg(r.tracksMatched == 1 ? "" : "s"));
    } else {
        m_offsetResult->setText(
            QString("Offset detection: %1")
                .arg(r.error.empty() ? "no match found" :
                     QString::fromStdString(r.error)));
    }
    m_offsetResult->setVisible(true);
}

void MainWindow::onTagsDoneUI(int count) {
    m_stateLabel->setText(QString("Tagging… (%1 track%2 tagged)")
        .arg(count).arg(count == 1 ? "" : "s"));
}

void MainWindow::onCompleteUI() {
    m_stateLabel->setText("Done!");
    m_trackProgress->setValue(100);
}

void MainWindow::onErrorUI(QString msg) {
    m_errorLabel->setText(msg);
    m_errorLabel->setVisible(true);
}

void MainWindow::onCancelledUI() {
    m_stateLabel->setText("Cancelled");
}

// ===========================================================================
// Close event
// ===========================================================================

void MainWindow::closeEvent(QCloseEvent* ev) {
    if (m_pipeline && m_pipeline->isRunning()) {
        m_pipeline->cancel();
        // Process events while waiting for the worker to stop
        while (m_pipeline && m_pipeline->isRunning())
            QApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    m_pipeline.reset();
    ev->accept();
}

} // namespace atomicripper::gui
