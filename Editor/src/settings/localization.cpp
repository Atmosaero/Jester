#include "settings/localization.h"
#include <QApplication>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QWidget>
static void initEditorTranslations() {
    static const bool once=[] {Q_INIT_RESOURCE(editor_translations);return true;}();Q_UNUSED(once);
}
namespace trickster {
LocalizationService::LocalizationService(QObject *parent):QObject(parent) {initEditorTranslations();}
LocalizationService &LocalizationService::instance() {
    static QPointer<LocalizationService> instance;
    if(!instance)instance=new LocalizationService(QCoreApplication::instance());
    return *instance;
}
QString LocalizationService::systemLanguage() {
    const auto languages=QLocale::system().uiLanguages();
    for(const auto &language:languages)
        if(language.startsWith("ru",Qt::CaseInsensitive))return "ru";
        else if(language.startsWith("en",Qt::CaseInsensitive))return "en";
    return "en";
}
bool LocalizationService::setPreference(const QString &preference,QString &error) {
    if(preference!="system" && preference!="en" && preference!="ru") {
        error="language.unsupported";return false;
    }
    const auto language=preference=="system"?systemLanguage():preference;
    if(language==active && initialized) {requested=preference;error.clear();return true;}
    if(language=="ru") {
        if(!own.load(":/jester/translations/jester_ru.qm") &&
           !own.load(QCoreApplication::applicationDirPath()+"/translations/jester_ru.qm")) {
            error="language.missing_jester_catalog";return false;
        }
        qtLoaded=qt.load("qtbase_ru",QCoreApplication::applicationDirPath()+"/translations") ||
                 qt.load("qtbase_ru",QLibraryInfo::path(QLibraryInfo::TranslationsPath));
        if(!qtLoaded) {error="language.missing_qt_catalog";return false;}
        QCoreApplication::installTranslator(&qt);
        QCoreApplication::installTranslator(&own);
    } else {
        QCoreApplication::removeTranslator(&own);
        QCoreApplication::removeTranslator(&qt);
        qtLoaded=false;
    }
    requested=preference;active=language;initialized=true;
    const QLocale locale(language=="ru"?QLocale(QLocale::Russian):QLocale(QLocale::English));
    QLocale::setDefault(locale); // QJsonDocument and API numbers remain locale-independent.
    if(auto *application=qobject_cast<QApplication*>(QCoreApplication::instance()))
        for(auto *window:application->topLevelWidgets()) {
            window->setLocale(locale);
            for(auto *widget:window->findChildren<QWidget*>())widget->setLocale(locale);
        }
    emit languageChanged();error.clear();return true;
}
QString uiText(const char *source,int count) {
    return QCoreApplication::translate("Jester",source,nullptr,count);
}
} // namespace trickster
