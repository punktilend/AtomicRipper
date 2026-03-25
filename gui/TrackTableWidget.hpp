#pragma once
#include <core/pipeline/Pipeline.hpp>
#include <core/verify/AccurateRip.hpp>

#include <QHash>
#include <QTableWidget>

namespace atomicripper::gui {

// ---------------------------------------------------------------------------
// TrackTableWidget — per-track results table
//
// Columns: #  |  Title  |  Duration  |  Status  |  CRC32  |  AR  |  C2
// ---------------------------------------------------------------------------
class TrackTableWidget : public QTableWidget {
    Q_OBJECT
public:
    explicit TrackTableWidget(QWidget* parent = nullptr);

    // Called once per disc load; resets and prepopulates rows.
    void populateFromToc(const drive::TOC& toc,
                         const metadata::MbRelease* release = nullptr);

    // Called from onTrackStart — highlights the active row.
    void setActiveTrack(int trackNumber);

    // Called from onTrackDone — fills status, CRC, C2 columns.
    void updateTrackDone(const pipeline::TrackDoneInfo& info);

    // Called from onVerifyDone — fills AR column for all tracks.
    void updateArResults(const verify::ArDiscResult& ar);

    // Reset to empty state.
    void reset();

private:
    enum Col { Number = 0, Title, Duration, Status, Crc32, Ar, C2, ColCount };

    QHash<int, int> m_trackRow;   // trackNumber → row index
};

} // namespace atomicripper::gui
