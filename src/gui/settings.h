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

    // DMR/BrandMeister identity -- separate from the D-Star callsign
    // identity above since DMR addresses by ID, not callsign. dmrPassword
    // is the repeater/hotspot password for dmrServer, not any kind of
    // account password; stored in the same plain JSON file as everything
    // else here (see settings.cpp's comment on why JSON over QSettings).
    uint32_t dmrId = 0;
    QString dmrPassword;
    QString dmrServer; // host:port, e.g. "3101.brandmeister.network:62031"
    unsigned dmrColorCode = 1;

    static QString filePath();
    static GuiSettings load();
    bool save() const;
};
