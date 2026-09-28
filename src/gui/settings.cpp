#include "settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

QString GuiSettings::filePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/settings.json";
}

QString GuiSettings::thumbdvTarget() const {
    if (thumbdvIsNetwork()) {
        if (thumbdvHost.isEmpty()) return QString();
        return QString("%1:%2").arg(thumbdvHost).arg(thumbdvPort);
    }
    return thumbdvDevice;
}

GuiSettings GuiSettings::load() {
    GuiSettings s;
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return s;

    QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    if (obj.contains("callsign")) s.callsign = obj["callsign"].toString();
    if (obj.contains("suffix")) s.suffix = obj["suffix"].toString();
    if (obj.contains("module_suffix")) s.moduleSuffix = obj["module_suffix"].toString();
    if (obj.contains("audio_input_device")) s.audioInputDevice = obj["audio_input_device"].toString();
    if (obj.contains("audio_output_device")) s.audioOutputDevice = obj["audio_output_device"].toString();
    if (obj.contains("thumbdv_device")) s.thumbdvDevice = obj["thumbdv_device"].toString();
    if (obj.contains("thumbdv_mode")) s.thumbdvMode = obj["thumbdv_mode"].toString() == "network" ? "network" : "serial";
    if (obj.contains("thumbdv_host")) s.thumbdvHost = obj["thumbdv_host"].toString();
    if (obj.contains("thumbdv_port")) s.thumbdvPort = qBound(1, obj["thumbdv_port"].toInt(), 65535);
    if (obj.contains("mic_volume")) s.micVolume = qBound(0, obj["mic_volume"].toInt(), 100);
    if (obj.contains("speaker_volume")) s.speakerVolume = qBound(0, obj["speaker_volume"].toInt(), 100);
    if (obj.contains("dmr_id")) s.dmrId = static_cast<uint32_t>(obj["dmr_id"].toDouble());
    if (obj.contains("dmr_network")) s.dmrNetwork = obj["dmr_network"].toString();
    // BrandMeister keeps the keys it had before TGIF existed, so existing
    // files load unchanged. The old "dmr_server"/"dmr_protocol" (BrandMeister
    // over Homebrew) are deliberately not read: that path is gone.
    if (obj.contains("dmr_password")) s.bmPassword = obj["dmr_password"].toString();
    if (obj.contains("dmr_open_terminal_server")) s.bmServer = obj["dmr_open_terminal_server"].toString();
    if (obj.contains("tgif_server")) s.tgifServer = obj["tgif_server"].toString();
    if (obj.contains("tgif_password")) s.tgifPassword = obj["tgif_password"].toString();
    if (obj.contains("dmr_id_suffix")) s.dmrIdSuffix = obj["dmr_id_suffix"].toString();
    if (obj.contains("dmr_latitude")) s.dmrLatitude = obj["dmr_latitude"].toDouble();
    if (obj.contains("dmr_longitude")) s.dmrLongitude = obj["dmr_longitude"].toDouble();
    if (obj.contains("dmr_location")) s.dmrLocation = obj["dmr_location"].toString();
    if (obj.contains("dmr_description")) s.dmrDescription = obj["dmr_description"].toString();
    if (obj.contains("dmr_url")) s.dmrUrl = obj["dmr_url"].toString();
    return s;
}

bool GuiSettings::save() const {
    QJsonObject obj;
    obj["callsign"] = callsign;
    obj["suffix"] = suffix;
    obj["module_suffix"] = moduleSuffix;
    obj["audio_input_device"] = audioInputDevice;
    obj["audio_output_device"] = audioOutputDevice;
    obj["thumbdv_device"] = thumbdvDevice;
    obj["thumbdv_mode"] = thumbdvMode;
    obj["thumbdv_host"] = thumbdvHost;
    obj["thumbdv_port"] = thumbdvPort;
    obj["mic_volume"] = micVolume;
    obj["speaker_volume"] = speakerVolume;
    obj["dmr_id"] = static_cast<double>(dmrId);
    obj["dmr_network"] = dmrNetwork;
    obj["dmr_password"] = bmPassword;
    obj["dmr_open_terminal_server"] = bmServer;
    obj["tgif_server"] = tgifServer;
    obj["tgif_password"] = tgifPassword;
    obj["dmr_id_suffix"] = dmrIdSuffix;
    obj["dmr_latitude"] = dmrLatitude;
    obj["dmr_longitude"] = dmrLongitude;
    obj["dmr_location"] = dmrLocation;
    obj["dmr_description"] = dmrDescription;
    obj["dmr_url"] = dmrUrl;

    QFile f(filePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}
