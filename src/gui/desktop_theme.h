#pragma once

// Following the desktop's light/dark preference on any Qt version.
//
// Qt 6.5 and later read it from the desktop themselves (the freedesktop
// portal's color-scheme), so a .deb using a recent distro's Qt comes up
// dark in dark mode. The AppImage bundles Debian 12's Qt 6.4, which
// doesn't, and came up light. So: ask the portal ourselves, and if it
// says "prefer dark" while Qt's palette is still light, switch to Fusion
// with a dark palette. Call once, after the QApplication is created and
// before any window is -- theme.h decides its colours from the palette.

class QApplication;

void followDesktopColorScheme(QApplication &app);
