#pragma once
#include "settings/editor_appearance.h"
#include "settings/preferences_service.h"
#include <QApplication>
#include <QDebug>
namespace trickster {
inline void applyAppearance(QApplication &app) {
    auto &service=PreferencesService::instance();
    applyEditorAppearance(app,service.current());
    QString error;if(!service.preview(service.current(),error))qWarning().noquote()<<error;
}
}
