#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "MainWindow.hpp"

#include <QApplication>
#include <QFont>

namespace {

const char* atomicThemeStyleSheet() {
    return R"(
        QWidget {
            background-color: #06110c;
            color: #d7ffe6;
            selection-background-color: #18d26b;
            selection-color: #021007;
            font-family: "Segoe UI";
        }

        QMainWindow, QDialog {
            background-color: #06110c;
        }

        QGroupBox {
            background-color: #0a1710;
            border: 1px solid #1f5a37;
            border-radius: 6px;
            margin-top: 18px;
            padding: 10px 8px 8px 8px;
            font-weight: 600;
            color: #8cffb5;
        }

        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 6px;
            background-color: #06110c;
        }

        QLabel {
            background: transparent;
            color: #d7ffe6;
        }

        QLineEdit, QComboBox, QSpinBox {
            background-color: #020805;
            color: #e6ffef;
            border: 1px solid #247a46;
            border-radius: 4px;
            padding: 4px 6px;
            min-height: 22px;
        }

        QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
            border: 1px solid #19e673;
        }

        QComboBox::drop-down {
            border: 0;
            width: 24px;
        }

        QComboBox QAbstractItemView {
            background-color: #06110c;
            color: #d7ffe6;
            border: 1px solid #247a46;
            selection-background-color: #18d26b;
            selection-color: #021007;
        }

        QPushButton {
            background-color: #0d2a19;
            color: #e6ffef;
            border: 1px solid #21a957;
            border-radius: 5px;
            padding: 5px 12px;
            font-weight: 600;
        }

        QPushButton:hover {
            background-color: #123923;
            border-color: #20e06c;
        }

        QPushButton:pressed {
            background-color: #18d26b;
            color: #021007;
        }

        QPushButton:disabled {
            background-color: #0a130e;
            color: #5d806a;
            border-color: #193522;
        }

        QCheckBox {
            background: transparent;
            spacing: 6px;
            color: #d7ffe6;
        }

        QCheckBox::indicator {
            width: 15px;
            height: 15px;
            border-radius: 3px;
            border: 1px solid #247a46;
            background-color: #020805;
        }

        QCheckBox::indicator:checked {
            background-color: #18d26b;
            border-color: #8cffb5;
        }

        QTableWidget {
            background-color: #020805;
            alternate-background-color: #07150d;
            color: #d7ffe6;
            gridline-color: #174428;
            border: 1px solid #1f5a37;
            border-radius: 4px;
        }

        QTableWidget::item:selected {
            background-color: #18d26b;
            color: #021007;
        }

        QHeaderView::section {
            background-color: #0d2a19;
            color: #8cffb5;
            border: 0;
            border-right: 1px solid #1f5a37;
            border-bottom: 1px solid #1f5a37;
            padding: 5px 6px;
            font-weight: 600;
        }

        QProgressBar {
            background-color: #020805;
            color: #d7ffe6;
            border: 1px solid #247a46;
            border-radius: 4px;
            text-align: center;
        }

        QProgressBar::chunk {
            background-color: #18d26b;
            border-radius: 3px;
        }

        QFrame[frameShape="4"], QFrame[frameShape="5"] {
            color: #1f5a37;
        }

        QToolTip {
            background-color: #0a1710;
            color: #e6ffef;
            border: 1px solid #21a957;
        }
    )";
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("AtomicRipper");
    app.setApplicationVersion("0.7.0");
    app.setOrganizationName("AtomicRipper");

    // Use a slightly larger default font on high-DPI screens
    QFont font = app.font();
    font.setPointSize(10);
    app.setFont(font);
    app.setStyleSheet(atomicThemeStyleSheet());

    atomicripper::gui::MainWindow win;
    win.show();

    return app.exec();
}
