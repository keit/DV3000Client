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
    // Optional 2-digit suffix appended to dmrId to form the 9-digit repeater
    // ID declared to BrandMeister -- needed when more than one hotspot/client
    // (e.g. BlueDV and this app) connect at once under the same DMR ID, since
    // each simultaneous connection must have a unique ID. Blank = use the
    // plain 7-digit dmrId. A QString (not a number) so "00" stays distinct
    // from blank.
    QString dmrIdSuffix;
    QString dmrServer; // host:port, e.g. "3101.brandmeister.network:62031"
    unsigned dmrColorCode = 1;
    // 1 or 2. Slot 2 is the confirmed convention for hotspot-style
    // BrandMeister connections (verified against a real, working Pi-Star
    // session's own config), hence the default -- see dmr_voice.h's
    // TimeSlot comment. Stored as plain 1/2 rather than dmr::TimeSlot to
    // keep this header free of a dependency on dmr_client.h.
    unsigned dmrTimeSlot = 2;

    // RPTC configuration fields -- mostly cosmetic/informational (shown on
    // the network's dashboard), but real masters (unlike xlxd's own
    // minimal RPTC handling) can validate them against your account's
    // actual registration and reject RPTC outright over a mismatch, e.g.
    // a generic placeholder frequency instead of your real one -- so
    // these default to plausible-but-generic values and are meant to be
    // overridden with your actual repeater/hotspot details (the same
    // numbers Pi-Star or BlueDV already has, if you run those).
    double dmrFrequencyMhz = 438.8;
    double dmrLatitude = 0.0;
    double dmrLongitude = 0.0;
    QString dmrLocation = "Unknown";
    QString dmrDescription = "DV3000Client";
    QString dmrUrl;

    static QString filePath();
    static GuiSettings load();
    bool save() const;
};
