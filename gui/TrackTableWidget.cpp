#include "TrackTableWidget.hpp"

#include <QColor>
#include <QFont>
#include <QHeaderView>
#include <QString>
#include <QTableWidgetItem>

namespace atomicripper::gui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TrackTableWidget::TrackTableWidget(QWidget* parent)
    : QTableWidget(parent)
{
    setColumnCount(ColCount);
    setHorizontalHeaderLabels({ "#", "Title", "Duration", "Status", "CRC32", "AR", "C2" });

    horizontalHeader()->setSectionResizeMode(Number,   QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Title,    QHeaderView::Stretch);
    horizontalHeader()->setSectionResizeMode(Duration, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Status,   QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Crc32,    QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(Ar,       QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(C2,       QHeaderView::ResizeToContents);

    verticalHeader()->setVisible(false);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TrackTableWidget::reset() {
    setRowCount(0);
    m_trackRow.clear();
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

        // Title — from MB if available
        QString title;
        if (release && audioIdx < static_cast<int>(release->tracks.size()))
            title = QString::fromStdString(
                release->tracks[static_cast<size_t>(audioIdx)].title);
        setItem(row, Title, makeItem(title, Qt::AlignLeft | Qt::AlignVCenter));

        // Duration from sector count
        const double secs = static_cast<double>(track.sectorCount) / 75.0;
        const int m = static_cast<int>(secs) / 60;
        const int s = static_cast<int>(secs) % 60;
        setItem(row, Duration,
                makeItem(QString::asprintf("%d:%02d", m, s)));

        // Remaining columns — blank until the rip fills them in
        setItem(row, Status, makeItem("—"));
        setItem(row, Crc32,  makeItem("—"));
        setItem(row, Ar,     makeItem("—"));
        setItem(row, C2,     makeItem("—"));

        ++audioIdx;
    }
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
