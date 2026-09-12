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

    QFile f(filePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}
