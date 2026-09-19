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
    if (obj.contains("dmr_id")) s.dmrId = static_cast<uint32_t>(obj["dmr_id"].toDouble());
    if (obj.contains("dmr_password")) s.dmrPassword = obj["dmr_password"].toString();
    if (obj.contains("dmr_id_suffix")) s.dmrIdSuffix = obj["dmr_id_suffix"].toString();
    if (obj.contains("dmr_server")) s.dmrServer = obj["dmr_server"].toString();
    if (obj.contains("dmr_color_code")) s.dmrColorCode = static_cast<unsigned>(obj["dmr_color_code"].toInt());
    if (obj.contains("dmr_time_slot")) s.dmrTimeSlot = static_cast<unsigned>(obj["dmr_time_slot"].toInt());
    if (obj.contains("dmr_frequency_mhz")) s.dmrFrequencyMhz = obj["dmr_frequency_mhz"].toDouble();
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
    obj["dmr_id"] = static_cast<double>(dmrId);
    obj["dmr_password"] = dmrPassword;
    obj["dmr_id_suffix"] = dmrIdSuffix;
    obj["dmr_server"] = dmrServer;
    obj["dmr_color_code"] = static_cast<int>(dmrColorCode);
    obj["dmr_time_slot"] = static_cast<int>(dmrTimeSlot);
    obj["dmr_frequency_mhz"] = dmrFrequencyMhz;
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
