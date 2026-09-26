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

    // Mic and speaker volume sliders on both protocol tabs (shared, so the
    // two tabs always agree), 0..100 with 50 = unity gain -- see
    // audio_gain.h. Adjusted live while communicating, unlike the device
    // choices above, so they're saved by the sliders themselves rather
    // than through the Settings dialog.
    int micVolume = 50;
    int speakerVolume = 50;

    // DMR identity. The DMR ID is one per operator, so it's shared by every
    // network; each network has its own server and its own password (TGIF's
    // is a key generated in your TGIF account, not your BrandMeister
    // Hotspot Security password). Passwords are stored in the same plain
    // JSON file as everything else here (see settings.cpp's comment on why
    // JSON over QSettings).
    uint32_t dmrId = 0;

    // Which network the DMR tab last connected to / the Settings dialog
    // opens on: "brandmeister" or "tgif". Each network is tied to one
    // protocol -- BrandMeister is reached via Open DMR Terminal, TGIF via
    // Homebrew/MMDVM -- so there's no separate protocol setting.
    QString dmrNetwork = "brandmeister";

    // BrandMeister (Open DMR Terminal).
    QString bmServer; // hostname of one of its masters, e.g. "3101.master.brandmeister.network"
    QString bmPassword;

    // TGIF (Homebrew/MMDVM). Everything from tgifPassword down to dmrUrl is
    // Homebrew-only: the RPTC packet declares a virtual repeater, which Open
    // DMR Terminal has no equivalent of.
    QString tgifServer = "tgif.network";
    QString tgifPassword; // the 16-digit key from your TGIF account's security page
    // Optional 2-digit suffix appended to dmrId to form the 9-digit repeater
    // ID declared to the master -- TGIF calls it the ESSID, and it's how more
    // than one hotspot/client can connect under the same DMR ID at once.
    // Blank = the plain 7-digit dmrId. A QString (not a number) so "00" stays
    // distinct from blank.
    QString dmrIdSuffix;

    // RPTC configuration fields -- mostly cosmetic/informational (shown on
    // the network's dashboard), but a real master (unlike xlxd's own minimal
    // RPTC handling) can reject RPTC outright over content it doesn't like
    // (see dmr_client.h's RepeaterConfig comment). The RF-only fields
    // (frequency, color code, time slot) are not settings at all -- they're
    // hardcoded in RepeaterConfig, since they mean nothing over IP.
    double dmrLatitude = 0.0;
    double dmrLongitude = 0.0;
    QString dmrLocation = "Unknown";
    QString dmrDescription = "DV3000Client";
    QString dmrUrl;

    static QString filePath();
    static GuiSettings load();
    bool save() const;
};
