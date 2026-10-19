#pragma once

// The few colours the GUI sets itself, for a light or a dark desktop theme.
// Everything else is left to the style, which follows the desktop -- so a
// colour set here must work against the theme's own text colour: in dark
// mode Qt draws text near-white, and on the light greys used in light
// mode, lists and labels became unreadable. Decided once, from the
// palette at startup; switching the desktop theme takes a restart.

#include <QApplication>
#include <QPalette>
#include <QString>

namespace theme {

inline bool dark() { return QApplication::palette().color(QPalette::Window).lightness() < 128; }

// Lists, tables and dropdown popups: a light grey a shade off white in
// light mode (easier on the eyes, but still reads as a content area
// against the window); in dark mode, the style's own background.
inline QString contentBackground(const QString &selector) {
    return dark() ? QString() : QString("%1 { background-color: #e8e8e8; }").arg(selector);
}

// The main window's brushed-metal gradient, behind every tab.
inline QString windowBackground() {
    return dark() ? "QMainWindow { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "  stop:0 #3c3f43, stop:0.5 #2f3134, stop:1 #24262a); }"
                  : "QMainWindow { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                    "  stop:0 #eef0f2, stop:0.5 #cdd0d3, stop:1 #aeb2b6); }";
}

inline QString menuBarBackground() { return dark() ? QString() : "QMenuBar { background-color: #e8e8e8; }"; }

// Status text: an amber for progress and state, red for errors, and the
// DMR tab's teal for the current subscription -- mid-dark shades on a light
// window, lighter ones on a dark window.
inline QString statusLabelStyle() { return dark() ? "color: #ffb74d;" : "color: #b36b00;"; }
inline QString errorLabelStyle() { return dark() ? "color: #ff6e6e;" : "color: #f44336;"; }
inline QString subscriptionLabelStyle() { return dark() ? "color: #4dd0e1;" : "color: #00838f;"; }

} // namespace theme
