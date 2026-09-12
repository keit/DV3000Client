#pragma once

// Persisted GUI settings: our callsign/module identity, audio device
// names, and the ThumbDV serial device path. Stored as JSON (not
// QSettings/.ini) per the project's step-1 request -- a plain, readable,
// diffable file rather than a registry-style key store.

#include <QString>

struct GuiSettings {
    QString callsign = "ZL2MIM";
    QString suffix; // MYCALL2 header field, e.g. a device/purpose suffix -- blank by default
    QString moduleSuffix = "B";
    QString audioInputDevice = "default";
    QString audioOutputDevice = "default";
    QString thumbdvDevice;

    static QString filePath();
    static GuiSettings load();
    bool save() const;
};
