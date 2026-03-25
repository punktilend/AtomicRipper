#include "ReleaseDialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

namespace atomicripper::gui {

ReleaseDialog::ReleaseDialog(const metadata::MbResult& result, QWidget* parent)
    : QDialog(parent)
    , m_count(static_cast<int>(result.releases.size()))
{
    setWindowTitle("Select Release");
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);

    auto* header = new QLabel(
        QString("MusicBrainz found <b>%1</b> matching release(s). "
                "Select the one that matches your disc:")
            .arg(m_count));
    header->setWordWrap(true);
    layout->addWidget(header);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);

    for (const auto& rel : result.releases) {
        QString year = rel.date.empty()
            ? "?"
            : QString::fromStdString(rel.date.substr(0, 4));

        QString country = rel.country.empty()
            ? QString()
            : QString(" [%1]").arg(QString::fromStdString(rel.country));

        QString label = rel.label.empty()
            ? QString()
            : QString(" · %1").arg(QString::fromStdString(rel.label));

        QString line = QString("%1 — %2  (%3)%4%5")
            .arg(QString::fromStdString(rel.title))
            .arg(QString::fromStdString(rel.artist))
            .arg(year)
            .arg(country)
            .arg(label);

        m_list->addItem(line);
    }

    if (m_list->count() > 0)
        m_list->setCurrentRow(0);

    // Double-click accepts
    connect(m_list, &QListWidget::itemActivated,
            this,   &ReleaseDialog::accept);

    layout->addWidget(m_list, /*stretch=*/1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

int ReleaseDialog::selectedIndex() const {
    if (!m_list || result() != QDialog::Accepted) return -1;
    return m_list->currentRow();
}

} // namespace atomicripper::gui
