#include "TrackTableWidget.hpp"

#include <QColor>
#include <QFont>
#include <QHeaderView>
#include <QString>
#include <QTableWidgetItem>

#include <utility>

namespace atomicripper::gui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TrackTableWidget::TrackTableWidget(QWidget* parent)
    : QTableWidget(parent)
{
    setColumnCount(ColCount);
    setHorizontalHeaderLabels({
        "#", "Title", "Artist", "Start", "Length", "Gap", "Size",
        "Compr. Size", "Pre-Emphasis", "Status", "CRC32", "AR", "C2"
    });

    horizontalHeader()->setSectionResizeMode(Number,         QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Title,          QHeaderView::Stretch);
    horizontalHeader()->setSectionResizeMode(Artist,         QHeaderView::Stretch);
    horizontalHeader()->setSectionResizeMode(Start,          QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Duration,       QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Gap,            QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Size,           QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(CompressedSize, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(PreEmphasis,    QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Status,         QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Crc32,          QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Ar,             QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(C2,             QHeaderView::ResizeToContents);

    verticalHeader()->setVisible(false);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    setAlternatingRowColors(true);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static QTableWidgetItem* makeItem(const QString& text,
                                  Qt::Alignment  align = Qt::AlignCenter) {
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(align);
    return item;
}

static QString lbaToMsf(uint32_t lba) {
    const uint32_t frames  = lba % 75;
    const uint32_t seconds = (lba / 75) % 60;
    const uint32_t minutes = lba / (75 * 60);
    return QString::asprintf("%02u:%02u:%02u", minutes, seconds, frames);
}

static QString sectorsToTime(uint32_t sectors) {
    return lbaToMsf(sectors);
}

static QString bytesToMb(uint64_t bytes) {
    return QString("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
}

static void setEditable(QTableWidgetItem* item, bool editable) {
    if (!item) return;
    Qt::ItemFlags flags = item->flags();
    if (editable)
        flags |= Qt::ItemIsEditable;
    else
        flags &= ~Qt::ItemIsEditable;
    item->setFlags(flags);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TrackTableWidget::reset() {
    setRowCount(0);
    m_trackRow.clear();
}

void TrackTableWidget::setMetadataEditingEnabled(bool enabled) {
    m_metadataEditingEnabled = enabled;
    for (int row = 0; row < rowCount(); ++row) {
        setEditable(item(row, Title), enabled);
        setEditable(item(row, Artist), enabled);
    }
}

void TrackTableWidget::populateFromToc(const drive::TOC&            toc,
                                        const metadata::MbRelease*  release) {
    reset();
    int audioIdx = 0;
    for (const auto& track : toc.tracks) {
        if (!track.isAudio) continue;

        const int row = rowCount();
        insertRow(row);
        m_trackRow[track.number] = row;

        // # column
        setItem(row, Number, makeItem(QString::number(track.number)));

        QString title = QString("Track%1").arg(track.number, 2, 10, QChar('0'));
        QString artist = release ? QString::fromStdString(release->artist) : QString("Unknown Artist");
        if (release && audioIdx < static_cast<int>(release->tracks.size())) {
            const auto& mbTrack = release->tracks[static_cast<size_t>(audioIdx)];
            if (!mbTrack.title.empty())
                title = QString::fromStdString(mbTrack.title);
            if (!mbTrack.artist.empty())
                artist = QString::fromStdString(mbTrack.artist);
        }
        auto* titleItem = makeItem(title, Qt::AlignLeft | Qt::AlignVCenter);
        auto* artistItem = makeItem(artist, Qt::AlignLeft | Qt::AlignVCenter);
        setEditable(titleItem, m_metadataEditingEnabled);
        setEditable(artistItem, m_metadataEditingEnabled);
        setItem(row, Title, titleItem);
        setItem(row, Artist, artistItem);

        // Duration from sector count
        setItem(row, Start, makeItem(lbaToMsf(track.lba)));
        setItem(row, Duration, makeItem(sectorsToTime(track.sectorCount)));
        setItem(row, Gap, makeItem("Unknown"));
        setItem(row, Size, makeItem(bytesToMb(static_cast<uint64_t>(track.sectorCount) * 2352u)));
        setItem(row, CompressedSize, makeItem(bytesToMb(
            static_cast<uint64_t>(static_cast<double>(track.sectorCount) * 2352.0 * 0.56))));
        setItem(row, PreEmphasis, makeItem("No"));

        // Remaining columns — blank until the rip fills them in
        setItem(row, Status, makeItem("—"));
        setItem(row, Crc32,  makeItem("—"));
        setItem(row, Ar,     makeItem("—"));
        setItem(row, C2,     makeItem("—"));

        ++audioIdx;
    }
}

metadata::MbRelease TrackTableWidget::buildReleaseFromRows(
    const drive::TOC& toc,
    const metadata::MbRelease& album) const
{
    metadata::MbRelease release = album;
    release.tracks.clear();

    int audioIdx = 0;
    for (const auto& track : toc.tracks) {
        if (!track.isAudio) continue;

        metadata::MbTrack mbTrack;
        mbTrack.number = track.number;
        mbTrack.title = QString("Track%1").arg(track.number, 2, 10, QChar('0')).toStdString();
        mbTrack.artist = release.artist;
        mbTrack.lengthMs = static_cast<int>(
            (static_cast<uint64_t>(track.sectorCount) * 1000u) / 75u);

        if (m_trackRow.contains(track.number)) {
            const int row = m_trackRow.value(track.number);
            if (auto* titleItem = item(row, Title)) {
                const QString title = titleItem->text().trimmed();
                if (!title.isEmpty())
                    mbTrack.title = title.toStdString();
            }
            if (auto* artistItem = item(row, Artist)) {
                const QString artist = artistItem->text().trimmed();
                if (!artist.isEmpty())
                    mbTrack.artist = artist.toStdString();
            }
        }

        if (mbTrack.artist.empty())
            mbTrack.artist = release.artist.empty() ? "Unknown Artist" : release.artist;

        release.tracks.push_back(std::move(mbTrack));
        ++audioIdx;
    }

    (void)audioIdx;
    return release;
}

void TrackTableWidget::setActiveTrack(int trackNumber) {
    // Un-bold all rows, bold + highlight the active one
    for (int r = 0; r < rowCount(); ++r) {
        for (int c = 0; c < ColCount; ++c) {
            if (auto* it = item(r, c)) {
                QFont f = it->font();
                f.setBold(false);
                it->setFont(f);
                it->setBackground(QBrush());  // default background
            }
        }
    }

    if (!m_trackRow.contains(trackNumber)) return;
    const int row = m_trackRow[trackNumber];
    for (int c = 0; c < ColCount; ++c) {
        if (auto* it = item(row, c)) {
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
        }
    }
    scrollToItem(item(row, 0));

    // Update status to "Ripping…"
    if (auto* st = item(row, Status)) {
        st->setText("Ripping…");
        st->setForeground(QColor("#2196F3"));  // blue
    }
}

void TrackTableWidget::updateTrackDone(const pipeline::TrackDoneInfo& info) {
    if (!m_trackRow.contains(info.trackNumber)) return;
    const int row = m_trackRow[info.trackNumber];

    // Status
    if (auto* st = item(row, Status)) {
        if (info.ok && info.suspectSectors == 0) {
            st->setText("OK");
            st->setForeground(QColor("#4CAF50"));   // green
        } else if (info.ok && info.suspectSectors > 0) {
            st->setText(QString("SUSPECT (%1)").arg(info.suspectSectors));
            st->setForeground(QColor("#FF9800"));   // orange
        } else {
            st->setText("FAILED");
            st->setForeground(QColor("#F44336"));   // red
        }
    }

    // CRC32
    if (auto* cr = item(row, Crc32))
        cr->setText(QString::asprintf("%08X", info.crc32));

    // C2 errors
    if (auto* c2 = item(row, C2)) {
        c2->setText(info.c2Sectors == 0 ? "0" : QString::number(info.c2Sectors));
        if (info.c2Sectors > 0)
            c2->setForeground(QColor("#FF9800"));
    }
}

void TrackTableWidget::updateArResults(const verify::ArDiscResult& ar) {
    if (!ar.lookupOk) {
        // Fill all AR cells with "N/A"
        for (int r = 0; r < rowCount(); ++r)
            if (auto* it = item(r, Ar)) it->setText("N/A");
        return;
    }

    for (const auto& tr : ar.tracks) {
        if (!m_trackRow.contains(tr.trackNumber)) continue;
        const int row = m_trackRow[tr.trackNumber];
        if (auto* it = item(row, Ar)) {
            if (tr.matched) {
                const int conf = tr.confidenceV2 > 0 ? tr.confidenceV2 : tr.confidenceV1;
                it->setText(QString("OK  conf=%1").arg(conf));
                it->setForeground(QColor("#4CAF50"));
            } else {
                it->setText("NO MATCH");
                it->setForeground(QColor("#F44336"));
            }
        }
    }
}

} // namespace atomicripper::gui
