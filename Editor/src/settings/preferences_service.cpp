#include "settings/preferences_service.h"
#include "settings/editor_appearance.h"
#include "settings/localization.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QSettings>

namespace trickster {
PreferencesService &PreferencesService::instance() {
    static QPointer<PreferencesService> service;
    if(!service)service=new PreferencesService(QCoreApplication::instance());
    return *service;
}
PreferencesService::PreferencesService(QObject *parent):QObject(parent) {
    path=qEnvironmentVariable("JESTER_PREFERENCES_FILE",
                              QCoreApplication::applicationDirPath()+"/user/editor-preferences.json");
    if(!QFileInfo::exists(path) && !qEnvironmentVariableIsSet("JESTER_PREFERENCES_FILE")) {
        const auto fallback=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/editor-preferences.json";
        if(QFileInfo::exists(fallback))path=fallback;
    }
    if(QFileInfo::exists(path))EditorPreferences::load(path,preferences,warning);
    if(qEnvironmentVariableIsSet("JESTER_PREFERENCES_FILE")) {
        // Test/isolated installations keep window state beside their explicit
        // preferences file rather than changing the normal Windows profile.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,QFileInfo(path).absolutePath()+"/qt-state");
    }
}
bool PreferencesService::preview(const EditorPreferences &value,QString &error) {
    if(!LocalizationService::instance().setPreference(value.value("language").toString(),error))return false;
    if(preferences==value) {error.clear();return true;}
    preferences=value;
    if(auto *application=qobject_cast<QApplication*>(QCoreApplication::instance()))
        applyEditorAppearance(*application,preferences);
    emit changed();error.clear();return true;
}
bool PreferencesService::apply(const EditorPreferences &value,QString &error) {
    if(!LocalizationService::instance().setPreference(value.value("language").toString(),error))return false;
    auto parent=QFileInfo(path).absolutePath();
    if(!QDir().mkpath(parent) || !value.save(path,error)) {
        // Installed applications can be read-only. Keep a per-user fallback;
        // neither location is exported as part of preferences.
        const auto fallback=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/editor-preferences.json";
        if(fallback==path || !QDir().mkpath(QFileInfo(fallback).absolutePath()) || !value.save(fallback,error))return false;
        path=fallback;
    }
    if(!preview(value,error))return false;warning.clear();error.clear();return true;
}
} // namespace trickster
