#pragma once
#include <core/metadata/MusicBrainz.hpp>

#include <QDialog>

class QListWidget;
class QLabel;

namespace atomicripper::gui {

// ---------------------------------------------------------------------------
// ReleaseDialog — modal release picker shown when MusicBrainz returns results
//
// Usage:
//   ReleaseDialog dlg(mbResult, parent);
//   if (dlg.exec() == QDialog::Accepted)
//       pipeline.selectRelease(dlg.selectedIndex());
//   else
//       pipeline.cancel();
// ---------------------------------------------------------------------------
class ReleaseDialog : public QDialog {
    Q_OBJECT
public:
    explicit ReleaseDialog(const metadata::MbResult& result,
                           QWidget* parent = nullptr);

    // 0-based index into MbResult::releases, or -1 if cancelled.
    int selectedIndex() const;

private:
    QListWidget* m_list   = nullptr;
    int          m_count  = 0;
};

} // namespace atomicripper::gui
