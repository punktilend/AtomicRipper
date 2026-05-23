#include "MainWindow.hpp"
#include "ReleaseDialog.hpp"
#include "TrackTableWidget.hpp"

#include <core/metadata/DiscId.hpp>

#include <algorithm>

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QSettings>
#include <QSizePolicy>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

namespace atomicripper::gui {

// ---------------------------------------------------------------------------
// TooltipDelegate — shows Qt::ToolTipRole text when hovering over combo items
// ---------------------------------------------------------------------------
class TooltipDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    bool helpEvent(QHelpEvent* ev, QAbstractItemView* view,
                   const QStyleOptionViewItem& option,
                   const QModelIndex& index) override
    {
        if (ev->type() == QEvent::ToolTip) {
            const QString tip = index.data(Qt::ToolTipRole).toString();
            if (!tip.isEmpty()) {
                QToolTip::showText(ev->globalPos(), tip, view);
                return true;
            }
        }
        return QStyledItemDelegate::helpEvent(ev, view, option, index);
    }
};

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
    ++m_previewScanSerial;
    if (m_previewThread.joinable())
        m_previewThread.join();
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
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setSpacing(10);

    auto* hbox = new QHBoxLayout();
    hbox->setSpacing(12);

    // ---- Left: rip settings ------------------------------------------------
    auto* ripBox = new QGroupBox("Rip Settings", page);
    auto* ripForm = new QVBoxLayout(ripBox);
    ripForm->setSpacing(6);

    // Output folder
    auto* dirRow = new QHBoxLayout();
    m_outputDir = new QLineEdit(ripBox);
    m_outputDir->setPlaceholderText("Output folder…");
    m_outputDir->setText(QDir::toNativeSeparators(
        QDir::homePath() + "/AtomicRipper/OutputFolder"));
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
    m_modeCombo->setItemData(0,
        "Re-reads each sector until two consecutive reads agree.\n"
        "Best balance of speed and accuracy. Recommended for most discs.",
        Qt::ToolTipRole);
    m_modeCombo->addItem("Burst");
    m_modeCombo->setItemData(1,
        "Single pass — reads each sector once and moves on.\n"
        "Fastest, but no error correction. Use only on pristine discs.",
        Qt::ToolTipRole);
    m_modeCombo->addItem("Paranoia");
    m_modeCombo->setItemData(2,
        "Like Secure but with stricter consensus requirements and more retries.\n"
        "Slowest — use when Secure still produces suspect sectors.",
        Qt::ToolTipRole);
    m_modeCombo->view()->setItemDelegate(new TooltipDelegate(m_modeCombo));
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
                              "Use AccurateRip configuration to detect and save this automatically.");
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
    m_chkRipLog      = makeChk("Write rip log");
    m_chkUploadInfo  = makeChk("Write upload helper");
    m_chkSingleFile  = makeChk("Single-file FLAC  (FLAC)", false);
    m_chkEject       = makeChk("Eject disc when done", false);

    optLayout->addStretch();
    hbox->addWidget(optBox);
    pageLayout->addLayout(hbox);

    // ---- Album metadata ---------------------------------------------------
    auto* metadataBox = new QGroupBox("Album Metadata", page);
    auto* metadataGrid = new QGridLayout(metadataBox);
    metadataGrid->setColumnStretch(1, 1);
    metadataGrid->setColumnStretch(3, 1);

    m_metadataSourceCombo = new QComboBox(metadataBox);
    m_metadataSourceCombo->addItem("MusicBrainz");
    m_metadataSourceCombo->addItem("CUETools DB");
    m_metadataSourceCombo->addItem("Manual / local TOC");
    m_metadataSourceCombo->setToolTip("Choose MusicBrainz, CUETools DB, or manual fields for album and track metadata.");

    m_albumTitleEdit = new QLineEdit(metadataBox);
    m_albumArtistEdit = new QLineEdit(metadataBox);
    m_albumYearEdit = new QLineEdit(metadataBox);
    m_albumGenreEdit = new QLineEdit(metadataBox);
    m_albumLabelEdit = new QLineEdit(metadataBox);
    m_albumCatalogEdit = new QLineEdit(metadataBox);
    m_albumComposerEdit = new QLineEdit(metadataBox);
    m_albumCommentEdit = new QLineEdit(metadataBox);

    m_albumTitleEdit->setPlaceholderText("Unknown Title");
    m_albumArtistEdit->setPlaceholderText("Unknown Artist");
    m_albumYearEdit->setPlaceholderText("Year");
    m_albumGenreEdit->setPlaceholderText("Genre");
    m_albumLabelEdit->setPlaceholderText("Record label");
    m_albumCatalogEdit->setPlaceholderText("Catalogue number");
    m_albumComposerEdit->setPlaceholderText("Composer");
    m_albumCommentEdit->setPlaceholderText("Comment");

    m_discNumberSpin = new QSpinBox(metadataBox);
    m_discNumberSpin->setRange(1, 99);
    m_discNumberSpin->setValue(1);
    m_discTotalSpin = new QSpinBox(metadataBox);
    m_discTotalSpin->setRange(1, 99);
    m_discTotalSpin->setValue(1);

    metadataGrid->addWidget(new QLabel("Source:", metadataBox), 0, 0);
    metadataGrid->addWidget(m_metadataSourceCombo, 0, 1);
    metadataGrid->addWidget(new QLabel("CD Title:", metadataBox), 1, 0);
    metadataGrid->addWidget(m_albumTitleEdit, 1, 1);
    metadataGrid->addWidget(new QLabel("CD Artist:", metadataBox), 2, 0);
    metadataGrid->addWidget(m_albumArtistEdit, 2, 1);
    metadataGrid->addWidget(new QLabel("Year:", metadataBox), 3, 0);
    metadataGrid->addWidget(m_albumYearEdit, 3, 1);
    metadataGrid->addWidget(new QLabel("Genre:", metadataBox), 0, 2);
    metadataGrid->addWidget(m_albumGenreEdit, 0, 3);
    metadataGrid->addWidget(new QLabel("Label:", metadataBox), 1, 2);
    metadataGrid->addWidget(m_albumLabelEdit, 1, 3);
    metadataGrid->addWidget(new QLabel("Catalogue #:", metadataBox), 2, 2);
    metadataGrid->addWidget(m_albumCatalogEdit, 2, 3);
    metadataGrid->addWidget(new QLabel("Composer:", metadataBox), 3, 2);
    metadataGrid->addWidget(m_albumComposerEdit, 3, 3);
    metadataGrid->addWidget(new QLabel("Comment:", metadataBox), 4, 0);
    metadataGrid->addWidget(m_albumCommentEdit, 4, 1);
    metadataGrid->addWidget(new QLabel("CD Number:", metadataBox), 4, 2);

    auto* discRow = new QHBoxLayout();
    discRow->addWidget(m_discNumberSpin);
    discRow->addWidget(new QLabel("of", metadataBox));
    discRow->addWidget(m_discTotalSpin);
    discRow->addStretch();
    metadataGrid->addLayout(discRow, 4, 3);

    pageLayout->addWidget(metadataBox);

    // ---- Disc preview -----------------------------------------------------
    auto* previewBox = new QGroupBox("Disc Preview", page);
    auto* previewLayout = new QVBoxLayout(previewBox);
    previewLayout->setSpacing(6);

    auto* previewTop = new QHBoxLayout();
    m_previewSummary = new QLabel("Insert a disc to preview it before ripping.", previewBox);
    m_previewSummary->setWordWrap(true);
    m_previewSummary->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    previewTop->addWidget(m_previewSummary);

    m_scanBtn = new QPushButton("Scan Disc", previewBox);
    m_scanBtn->setToolTip("Read the disc table of contents and look up metadata before ripping");
    m_scanBtn->setEnabled(false);
    previewTop->addWidget(m_scanBtn);
    previewLayout->addLayout(previewTop);

    m_previewDiscId = new QLabel(previewBox);
    m_previewDiscId->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewLayout->addWidget(m_previewDiscId);

    auto* releaseRow = new QHBoxLayout();
    releaseRow->addWidget(new QLabel("Matched release:", previewBox));
    m_previewReleaseCombo = new QComboBox(previewBox);
    m_previewReleaseCombo->setEnabled(false);
    releaseRow->addWidget(m_previewReleaseCombo, /*stretch=*/1);
    previewLayout->addLayout(releaseRow);

    m_previewMetadataStatus = new QLabel("Metadata lookup has not run yet.", previewBox);
    m_previewMetadataStatus->setWordWrap(true);
    previewLayout->addWidget(m_previewMetadataStatus);

    m_previewTable = new TrackTableWidget(previewBox);
    m_previewTable->setMetadataEditingEnabled(true);
    m_previewTable->setMinimumHeight(150);
    previewLayout->addWidget(m_previewTable, /*stretch=*/1);

    pageLayout->addWidget(previewBox, /*stretch=*/1);

    // Connections
    connect(browseBtn,     &QPushButton::clicked,
            this,          &MainWindow::browseOutputDir);
    connect(m_outputDir,   &QLineEdit::textChanged,
            this,          [this](const QString&) { updateStartButton(); });
    connect(m_formatCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this,          &MainWindow::onFormatChanged);
    connect(m_chkMetadata, &QCheckBox::toggled,
            this,          [this](bool on) {
                m_metadataSourceCombo->setEnabled(on);
                m_chkAutoSelect->setEnabled(on);
                m_chkCoverArt->setEnabled(on && m_formatCombo->currentIndex() == 0);
                m_chkWriteTags->setEnabled(on && m_formatCombo->currentIndex() == 0);
                if (on && m_hasToc && m_metadataSourceCombo->currentIndex() != 2)
                    startMetadataPreview();
                if (!on && m_hasToc) {
                    m_previewReleases.clear();
                    m_previewReleaseCombo->clear();
                    m_previewReleaseCombo->setEnabled(false);
                    m_previewMetadataStatus->setText("Internet metadata lookup is disabled.");
                    m_previewTable->populateFromToc(m_currentToc);
                }
            });
    connect(m_metadataSourceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (!m_chkMetadata->isChecked() || !m_hasToc)
                    return;
                if (index == 0 || index == 1) {
                    startMetadataPreview();
                } else {
                    m_previewReleases.clear();
                    m_previewReleaseCombo->clear();
                    m_previewReleaseCombo->setEnabled(false);
                    m_previewMetadataStatus->setText("Manual metadata mode: edit the album fields and track rows before ripping.");
                    m_previewTable->populateFromToc(m_currentToc, nullptr);
                }
            });
    connect(m_scanBtn, &QPushButton::clicked,
            this,      &MainWindow::scanCurrentDisc);
    connect(m_previewReleaseCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this,                  &MainWindow::onPreviewReleaseChanged);
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
    ++m_previewScanSerial;
    m_hasToc = false;
    m_currentToc = {};
    m_previewReleases.clear();
    m_discInfoLabel->setText("Checking…");
    clearDiscPreview("Checking selected drive…");
    updateStartButton();

    if (index < 0 || index >= static_cast<int>(m_drives.size())) {
        m_discInfoLabel->setText("No drive selected");
        clearDiscPreview("No drive selected.");
        return;
    }

    const auto& drv = m_drives[static_cast<size_t>(index)];
    loadSavedAccurateRipOffset();

    const auto status = drv.status();

    if (status != drive::DriveStatus::Ready) {
        m_discInfoLabel->setText(
            status == drive::DriveStatus::Empty ? "No disc" : "Drive not ready");
        clearDiscPreview(status == drive::DriveStatus::Empty
            ? "No disc inserted. Insert an audio CD to preview its contents."
            : "Drive is not ready.");
        updateStartButton();
        return;
    }

    auto tocOpt = drv.readTOC();
    if (!tocOpt || !tocOpt->isValid()) {
        m_discInfoLabel->setText("Could not read TOC");
        clearDiscPreview("Could not read the disc table of contents.");
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

    showLocalDiscPreview();
    if (m_chkMetadata->isChecked() && m_metadataSourceCombo->currentIndex() != 2)
        startMetadataPreview();
    maybeOfferAccurateRipConfiguration();

    updateStartButton();
}

void MainWindow::pollDiscStatus() {
    if (m_closing) return;
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

void MainWindow::scanCurrentDisc() {
    onDriveChanged(m_driveCombo->currentIndex());
}

// ===========================================================================
// Configuration helpers
// ===========================================================================

std::string MainWindow::selectedDrivePath() const {
    const int idx = m_driveCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_drives.size())) return {};
    return m_drives[static_cast<size_t>(idx)].path();
}

QString MainWindow::selectedDriveSettingsKey() const {
    const int idx = m_driveCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_drives.size())) return {};

    const auto& drv = m_drives[static_cast<size_t>(idx)];
    const QString raw = QString::fromStdString(drv.path() + "|" + drv.description());
    return QString::fromLatin1(raw.toUtf8().toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool MainWindow::hasSavedAccurateRipOffset(const QString& driveKey) const {
    if (driveKey.isEmpty()) return false;
    QSettings settings;
    return settings.contains(QString("accuraterip/driveOffsets/%1").arg(driveKey));
}

void MainWindow::loadSavedAccurateRipOffset() {
    const QString driveKey = selectedDriveSettingsKey();
    if (driveKey.isEmpty()) return;

    QSettings settings;
    const QString settingKey = QString("accuraterip/driveOffsets/%1").arg(driveKey);
    const QSignalBlocker blocker(m_offsetSpin);
    m_offsetSpin->setValue(settings.value(settingKey, 0).toInt());
}

void MainWindow::maybeOfferAccurateRipConfiguration() {
    if (!m_hasToc || !m_chkAccurateRip || !m_chkAccurateRip->isChecked())
        return;

    const QString driveKey = selectedDriveSettingsKey();
    if (driveKey.isEmpty() || hasSavedAccurateRipOffset(driveKey))
        return;

    const QString discKey = QString("%1-%2-%3")
        .arg(m_currentToc.audioTrackCount())
        .arg(verify::AccurateRip::discId1(m_currentToc), 8, 16, QChar('0'))
        .arg(verify::AccurateRip::discId2(m_currentToc), 8, 16, QChar('0'));
    const QString promptKey = driveKey + "/" + discKey;
    if (m_lastAccurateRipPromptKey == promptKey)
        return;
    m_lastAccurateRipPromptKey = promptKey;

    QTimer::singleShot(0, this, [this]() {
        if (!m_hasToc || m_closing || (m_pipeline && m_pipeline->isRunning()))
            return;

        const int idx = m_driveCombo->currentIndex();
        const QString driveName =
            (idx >= 0 && idx < static_cast<int>(m_drives.size()))
                ? QString::fromStdString(m_drives[static_cast<size_t>(idx)].description())
                : QString("Selected drive");

        QMessageBox dlg(this);
        dlg.setWindowTitle("Configure AccurateRip");
        dlg.setIcon(QMessageBox::Information);
        dlg.setText("This Audio CD can be used to configure AccurateRip.");
        dlg.setInformativeText(
            QString("AtomicRipper can detect and save the read offset for this drive during the next rip.\n\n"
                    "CD Drive Type: %1\n"
                    "Expected Offset: detected from AccurateRip after ripping\n\n"
                    "Configure AccurateRip now?")
                .arg(driveName));
        QPushButton* configureBtn = dlg.addButton("Configure", QMessageBox::AcceptRole);
        dlg.addButton("Close", QMessageBox::RejectRole);
        dlg.setDefaultButton(configureBtn);
        dlg.exec();

        if (dlg.clickedButton() == configureBtn) {
            m_chkAccurateRip->setChecked(true);
            m_chkDetectOff->setChecked(true);
            m_pendingAccurateRipConfig = true;
            m_previewMetadataStatus->setText(
                "AccurateRip configuration is armed. Start the rip to detect and save this drive's offset.");
        }
    });
}

void MainWindow::saveAccurateRipOffset(int offset) {
    const QString driveKey = selectedDriveSettingsKey();
    if (driveKey.isEmpty()) return;

    QSettings settings;
    settings.setValue(QString("accuraterip/driveOffsets/%1").arg(driveKey), offset);
    settings.sync();

    const QSignalBlocker blocker(m_offsetSpin);
    m_offsetSpin->setValue(offset);
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

    cfg.fetchMetadata     = m_chkMetadata->isChecked() && m_metadataSourceCombo->currentIndex() == 0;
    cfg.autoSelectRelease = m_chkAutoSelect->isChecked();
    cfg.embedCoverArt     = isFlac && m_chkCoverArt->isChecked();
    cfg.writeTags         = isFlac && m_chkWriteTags->isChecked();
    cfg.verifyAccurateRip = m_chkAccurateRip->isChecked();
    cfg.autoDetectOffset  = m_chkDetectOff->isChecked();
    cfg.writeCueSheet     = m_chkCueSheet->isChecked();
    cfg.writeRipLog       = m_chkRipLog->isChecked();
    cfg.writeUploadInfo   = m_chkUploadInfo->isChecked();
    cfg.singleFile        = isFlac && m_chkSingleFile->isChecked();
    cfg.ejectWhenDone     = m_chkEject->isChecked();
    cfg.manualRelease     = collectManualRelease();
    cfg.useManualMetadata = m_hasToc && !cfg.manualRelease.tracks.empty();

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

void MainWindow::clearDiscPreview(const QString& message) {
    if (m_previewSummary)
        m_previewSummary->setText(message);
    if (m_previewDiscId)
        m_previewDiscId->clear();
    if (m_previewMetadataStatus)
        m_previewMetadataStatus->setText("Metadata lookup has not run yet.");
    if (m_previewReleaseCombo) {
        QSignalBlocker blocker(m_previewReleaseCombo);
        m_previewReleaseCombo->clear();
        m_previewReleaseCombo->setEnabled(false);
    }
    if (m_previewTable)
        m_previewTable->reset();
    if (m_scanBtn)
        m_scanBtn->setEnabled(false);
}

void MainWindow::showLocalDiscPreview() {
    if (!m_hasToc) return;

    const int audioTracks = m_currentToc.audioTrackCount();
    const int totalTracks = static_cast<int>(m_currentToc.tracks.size());
    const double secs = m_currentToc.durationSeconds();
    const int minutes = static_cast<int>(secs) / 60;
    const int seconds = static_cast<int>(secs) % 60;
    const std::string discId = metadata::DiscId::calculate(m_currentToc);

    m_previewSummary->setText(
        QString("Local disc scan: %1 audio track%2, %3 total track%4, %5:%6 runtime. Review this before ripping.")
            .arg(audioTracks)
            .arg(audioTracks == 1 ? "" : "s")
            .arg(totalTracks)
            .arg(totalTracks == 1 ? "" : "s")
            .arg(minutes)
            .arg(seconds, 2, 10, QChar('0')));

    m_previewDiscId->setText(QString("MusicBrainz Disc ID: %1")
        .arg(QString::fromStdString(discId.empty() ? std::string("(not available)") : discId)));

    m_albumTitleEdit->setText("Unknown Title");
    m_albumArtistEdit->setText("Unknown Artist");
    m_albumYearEdit->clear();
    m_albumGenreEdit->clear();
    m_albumLabelEdit->clear();
    m_albumCatalogEdit->clear();
    m_albumComposerEdit->clear();
    m_albumCommentEdit->clear();
    m_discNumberSpin->setValue(1);
    m_discTotalSpin->setValue(1);
    setWindowTitle("AtomicRipper - Unknown Artist / Unknown Title");

    m_previewMetadataStatus->setText(m_chkMetadata->isChecked() && m_metadataSourceCombo->currentIndex() != 2
        ? QString("Looking up %1 releases and track titles…").arg(m_metadataSourceCombo->currentText())
        : "Manual metadata mode. Edit the album fields and track rows before ripping.");

    {
        QSignalBlocker blocker(m_previewReleaseCombo);
        m_previewReleaseCombo->clear();
        m_previewReleaseCombo->setEnabled(false);
    }
    m_previewTable->populateFromToc(m_currentToc);
    m_scanBtn->setEnabled(true);
}

void MainWindow::startMetadataPreview() {
    if (!m_hasToc || m_metadataSourceCombo->currentIndex() == 2) return;

    const std::string discId = metadata::DiscId::calculate(m_currentToc);
    if (discId.empty()) {
        m_previewMetadataStatus->setText("MusicBrainz lookup skipped: no disc ID could be calculated.");
        return;
    }

    const int serial = ++m_previewScanSerial;
    if (m_previewThread.joinable())
        m_previewThread.join();

    const int sourceIndex = m_metadataSourceCombo->currentIndex();
    const QString sourceName = m_metadataSourceCombo->currentText();
    m_previewMetadataStatus->setText(
        QString("Looking up %1 releases and track titles…").arg(sourceName));
    m_previewReleaseCombo->clear();
    m_previewReleaseCombo->setEnabled(false);
    m_previewReleases.clear();

    drive::TOC toc = m_currentToc;
    m_previewThread = std::thread([this, serial, discId, toc, sourceIndex]() {
        metadata::MbResult result = sourceIndex == 1
            ? metadata::CueToolsDB::lookup(toc)
            : metadata::MusicBrainz::lookup(discId, toc);
        QMetaObject::invokeMethod(this, [this, serial, result = std::move(result)]() mutable {
            applyMetadataPreview(serial, std::move(result));
        }, Qt::QueuedConnection);
    });
}

void MainWindow::applyMetadataPreview(int serial, metadata::MbResult result) {
    if (serial != m_previewScanSerial.load() || !m_hasToc)
        return;

    if (!result.ok) {
        m_previewMetadataStatus->setText(
            QString("%1 lookup failed: %2")
                .arg(m_metadataSourceCombo->currentText())
                .arg(QString::fromStdString(result.error.empty() ? "unknown error" : result.error)));
        return;
    }

    if (result.releases.empty()) {
        m_previewMetadataStatus->setText(result.error.empty()
            ? QString("%1 did not find a matching release. The local TOC is still available for ripping.")
                .arg(m_metadataSourceCombo->currentText())
            : QString("%1: %2. The local TOC is still available for ripping.")
                .arg(m_metadataSourceCombo->currentText())
                .arg(QString::fromStdString(result.error)));
        return;
    }

    m_previewReleases = std::move(result.releases);
    {
        QSignalBlocker blocker(m_previewReleaseCombo);
        m_previewReleaseCombo->clear();
        for (const auto& rel : m_previewReleases) {
            const QString year = rel.date.empty()
                ? "?"
                : QString::fromStdString(rel.date.substr(0, 4));
            const QString country = rel.country.empty()
                ? QString()
                : QString(" [%1]").arg(QString::fromStdString(rel.country));
            m_previewReleaseCombo->addItem(
                QString("%1 — %2 (%3)%4")
                    .arg(QString::fromStdString(rel.title))
                    .arg(QString::fromStdString(rel.artist))
                    .arg(year)
                    .arg(country));
        }
        m_previewReleaseCombo->setCurrentIndex(0);
        m_previewReleaseCombo->setEnabled(m_previewReleaseCombo->count() > 1);
    }

    m_previewMetadataStatus->setText(
        QString("%1 found %2 matching release%3. Pick the one that matches the disc before ripping.")
            .arg(m_metadataSourceCombo->currentText())
            .arg(m_previewReleases.size())
            .arg(m_previewReleases.size() == 1 ? "" : "s"));
    onPreviewReleaseChanged(0);
}

void MainWindow::onPreviewReleaseChanged(int index) {
    if (!m_hasToc || index < 0 || index >= static_cast<int>(m_previewReleases.size()))
        return;
    const auto& release = m_previewReleases[static_cast<size_t>(index)];
    applyReleaseToMetadataFields(release);
    m_previewTable->populateFromToc(m_currentToc, &release);
}

metadata::MbRelease MainWindow::collectManualRelease() const {
    metadata::MbRelease album;
    album.title = m_albumTitleEdit->text().trimmed().isEmpty()
        ? "Unknown Title"
        : m_albumTitleEdit->text().trimmed().toStdString();
    album.artist = m_albumArtistEdit->text().trimmed().isEmpty()
        ? "Unknown Artist"
        : m_albumArtistEdit->text().trimmed().toStdString();
    album.date = m_albumYearEdit->text().trimmed().toStdString();
    album.genre = m_albumGenreEdit->text().trimmed().toStdString();
    album.label = m_albumLabelEdit->text().trimmed().toStdString();
    album.catalogNumber = m_albumCatalogEdit->text().trimmed().toStdString();
    album.composer = m_albumComposerEdit->text().trimmed().toStdString();
    album.comment = m_albumCommentEdit->text().trimmed().toStdString();
    album.discNumber = m_discNumberSpin->value();
    album.totalDiscs = m_discTotalSpin->value();

    if (!m_hasToc || !m_previewTable)
        return album;

    album = m_previewTable->buildReleaseFromRows(m_currentToc, album);
    return album;
}

void MainWindow::applyReleaseToMetadataFields(const metadata::MbRelease& release) {
    m_albumTitleEdit->setText(QString::fromStdString(
        release.title.empty() ? std::string("Unknown Title") : release.title));
    m_albumArtistEdit->setText(QString::fromStdString(
        release.artist.empty() ? std::string("Unknown Artist") : release.artist));
    m_albumYearEdit->setText(QString::fromStdString(release.date.substr(0, 4)));
    m_albumGenreEdit->clear();
    m_albumLabelEdit->setText(QString::fromStdString(release.label));
    m_albumCatalogEdit->setText(QString::fromStdString(release.catalogNumber));
    m_albumComposerEdit->clear();
    m_albumCommentEdit->clear();
    m_discNumberSpin->setValue(std::max(1, release.discNumber));
    m_discTotalSpin->setValue(std::max(1, release.totalDiscs));
    setWindowTitle(QString("AtomicRipper - %1 / %2")
        .arg(m_albumArtistEdit->text())
        .arg(m_albumTitleEdit->text()));
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

    // Pre-populate track table from the known TOC and current metadata fields.
    m_trackTable->populateFromToc(m_currentToc, &cfg.manualRelease);

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
            if (!m_closing) {
                m_stack->setCurrentIndex(0);
                m_cancelBtn->setVisible(false);
                m_discPollTimer->start();
                onDriveChanged(m_driveCombo->currentIndex());
                updateStartButton();
            }
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

    if (m_pendingAccurateRipConfig && ar.lookupOk) {
        int matched = 0;
        for (const auto& track : ar.tracks) {
            if (track.matched)
                ++matched;
        }

        if (matched >= 2) {
            saveAccurateRipOffset(m_offsetSpin->value());
            m_pendingAccurateRipConfig = false;
            m_chkDetectOff->setChecked(false);
            m_offsetResult->setText(
                QString("AccurateRip configured: saved drive offset <b>%1%2</b> samples.")
                    .arg(m_offsetSpin->value() >= 0 ? "+" : "")
                    .arg(m_offsetSpin->value()));
            m_offsetResult->setVisible(true);
        } else if (matched > 0) {
            m_pendingAccurateRipConfig = false;
            m_chkDetectOff->setChecked(false);
            const QString message =
                "Offset values did not match across tracks; possibly this disc has scratches. Please try another disc.";
            m_offsetResult->setText(message);
            m_offsetResult->setVisible(true);
            QMessageBox::information(this, "AccurateRip", message);
        }
    }
}

void MainWindow::onOffsetDetectedUI(verify::ArOffsetResult r) {
    if (r.found) {
        saveAccurateRipOffset(r.sampleOffset);
        m_offsetResult->setText(
            QString("Detected and saved drive offset: <b>%1%2</b> samples  "
                    "(conf=%3, %4 track%5 matched). "
                    "Future rips from this drive will use this offset.")
                .arg(r.sampleOffset >= 0 ? "+" : "")
                .arg(r.sampleOffset)
                .arg(r.confidence)
                .arg(r.tracksMatched)
                .arg(r.tracksMatched == 1 ? "" : "s"));
        m_pendingAccurateRipConfig = false;
        m_chkDetectOff->setChecked(false);
    } else {
        const QString message = r.error.empty()
            ? QString("no match found")
            : QString::fromStdString(r.error);
        m_offsetResult->setText(QString("Offset detection: %1").arg(message));
        if (m_pendingAccurateRipConfig) {
            m_pendingAccurateRipConfig = false;
            QMessageBox::information(this, "AccurateRip", message);
        }
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
    m_closing = true;
    m_discPollTimer->stop();   // prevent any further drive polls
    if (m_pipeline) {
        m_pipeline->cancel();
        m_pipeline.reset();    // joins the worker thread
    }
    ev->accept();
}

} // namespace atomicripper::gui
