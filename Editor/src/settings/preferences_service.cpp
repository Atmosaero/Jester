#include "settings/preferences_service.h"
#include "settings/editor_appearance.h"
#include "settings/blue_jay_style.h"
#include "settings/localization.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QSettings>
#include <QMap>

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
    // Migrate only the former factory colors, once per local profile. An
    // explicit custom accent and later imports remain user choices.
    QSettings settings;
    if(!settings.value("appearance/blueJayFactoryColors",false).toBool()) {
        QJsonObject appearance,overlays;
        if(preferences.value("appearance.accent")=="#d7a64f")appearance["accent"]=BlueJay::Blue;
        const QMap<QString,QString> factory{{"bones","#8ea9bc"},{"selection","#efbb62"},{"notes","#ecd58a"},
            {"trajectory","#62bdb4"},{"manipulatorX","#df7168"},{"manipulatorY","#72c58a"},{"manipulatorZ","#7ba5e2"}};
        const EditorPreferences defaults;
        for(auto it=factory.begin();it!=factory.end();++it)
            if(preferences.value("overlays."+it.key())==it.value())overlays[it.key()]=defaults.value("overlays."+it.key());
        QJsonObject patch;if(!appearance.isEmpty())patch["appearance"]=appearance;if(!overlays.isEmpty())patch["overlays"]=overlays;
        QString error;
        if(!patch.isEmpty() && preferences.patch(patch,error) && QFileInfo::exists(path))
            if(!preferences.save(path,error))warning=error;
        settings.setValue("appearance/blueJayFactoryColors",true);
    }
    // Adopt the revised Blue Jay factory accent in existing red-theme profiles.
    // Subsequent explicit color choices remain intact.
    if(!settings.value("appearance/blueJayBlueAccent",false).toBool()) {
        QJsonObject patch;
        if(QColor(preferences.value("appearance.accent").toString())==QColor(BlueJay::Red))
            patch["appearance"]=QJsonObject{{"accent",BlueJay::Blue}};
        if(QColor(preferences.value("overlays.selection").toString())==QColor(BlueJay::Red))
            patch["overlays"]=QJsonObject{{"selection",BlueJay::Blue}};
        QString error;bool persisted=true;
        if(!patch.isEmpty()) {
            persisted=preferences.patch(patch,error);
            if(persisted && QFileInfo::exists(path))persisted=preferences.save(path,error);
            if(!persisted)warning=error;
        }
        if(persisted)settings.setValue("appearance/blueJayBlueAccent",true);
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
