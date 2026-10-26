#include "settings/localization.h"
#include "settings/owned_message_sources.h"
#include "ui/widgets/localized_ui.h"
#include <QApplication>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QRegularExpression>
#include <QHash>
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
    if(!languages.isEmpty() && languages.front().startsWith("ru",Qt::CaseInsensitive))return "ru";
    return "en";
}
bool LocalizationService::setPreference(const QString &preference,QString &error) {
    if(preference!="system" && preference!="en" && preference!="ru") {
        error="language.unsupported";return false;
    }
    const auto language=preference=="system"?systemLanguage():preference;
    if(language==active && initialized) {requested=preference;error.clear();return true;}
    QList<std::function<void()>> restoreDrafts;
    if(auto *application=qobject_cast<QApplication*>(QCoreApplication::instance()))
        for(auto *window:application->topLevelWidgets())restoreDrafts.push_back(preserveUiDrafts(*window));
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
        if(!own.load(":/jester/translations/jester_en.qm") &&
           !own.load(QCoreApplication::applicationDirPath()+"/translations/jester_en.qm")) {
            error="language.missing_jester_catalog";return false;
        }
        QCoreApplication::installTranslator(&own);
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
    emit languageChanged();for(const auto &restore:restoreDrafts)restore();error.clear();return true;
}
QString ownedValueLabel(const QString &value) {
    static const QMap<QString,QString> sources{
        {"ready",QT_TRANSLATE_NOOP("Jester","Ready")},{"running",QT_TRANSLATE_NOOP("Jester","Running")},
        {"idle",QT_TRANSLATE_NOOP("Jester","Idle")},{"stale",QT_TRANSLATE_NOOP("Jester","Stale")},
        {"cancelled",QT_TRANSLATE_NOOP("Jester","Cancelled")},{"failed",QT_TRANSLATE_NOOP("Jester","Failed")},
        {"pass",QT_TRANSLATE_NOOP("Jester","Pass")},{"fail",QT_TRANSLATE_NOOP("Jester","Fail")},
        {"confirmed",QT_TRANSLATE_NOOP("Jester","Confirmed")},{"undefined",QT_TRANSLATE_NOOP("Jester","Undefined")},
        {"undefined-at-discontinuity",QT_TRANSLATE_NOOP("Jester","Undefined at discontinuity")},
        {"budget-exhausted",QT_TRANSLATE_NOOP("Jester","Budget exhausted")},
        {"joint-limit",QT_TRANSLATE_NOOP("Jester","Joint limit")},{"proxy-overlap",QT_TRANSLATE_NOOP("Jester","Proxy overlap")},
        {"surface-component-containment",QT_TRANSLATE_NOOP("Jester","Surface containment")},
        {"surface-self-intersection",QT_TRANSLATE_NOOP("Jester","Surface self-intersection")},
        {"metres",QT_TRANSLATE_NOOP("Jester","Metres")},{"scene",QT_TRANSLATE_NOOP("Jester","Scene units")},
        {"degrees",QT_TRANSLATE_NOOP("Jester","Degrees")},
        {"attached",QT_TRANSLATE_NOOP("Jester","Attached")},
        {"ambiguous-retained",QT_TRANSLATE_NOOP("Jester","Ambiguous key retained")},
        {"orphaned-retained",QT_TRANSLATE_NOOP("Jester","Missing key retained")}};
    if(sources.contains(value))return uiText(sources[value]);
    return translatedDiagnostic(value);
}
QString translatedDiagnostic(const QString &message) {
    // Called only at owned diagnostic/history presentation boundaries, never
    // for authored text or values returned by the machine API.
    if(message.startsWith("preferences.") || message.startsWith("visual.") || message.startsWith("language.")) {
        const auto split=message.indexOf(": ");
        const auto code=split<0?message:message.left(split),detail=split<0?QString():message.mid(split+2);
        static const QMap<QString,QString> labels{
            {"visual.unknown_parameter",QT_TRANSLATE_NOOP("Jester","Unknown scene setting")},
            {"visual.expected_object",QT_TRANSLATE_NOOP("Jester","Expected a settings group")},
            {"visual.invalid_value",QT_TRANSLATE_NOOP("Jester","Invalid scene setting value")},
            {"visual.unsupported_parameter",QT_TRANSLATE_NOOP("Jester","Unsupported scene setting")},
            {"visual.unsupported_version",QT_TRANSLATE_NOOP("Jester","Unsupported scene settings version")},
            {"visual.transaction_in_progress",QT_TRANSLATE_NOOP("Jester","Finish the current edit first.")},
            {"preferences.unknown_parameter",QT_TRANSLATE_NOOP("Jester","Unknown editor preference")},
            {"preferences.expected_object",QT_TRANSLATE_NOOP("Jester","Expected a preference group")},
            {"preferences.unsupported_format_or_version",QT_TRANSLATE_NOOP("Jester","Unsupported preferences format or version")},
            {"preferences.invalid_language",QT_TRANSLATE_NOOP("Jester","Unsupported language")},
            {"preferences.invalid_appearance",QT_TRANSLATE_NOOP("Jester","Invalid appearance settings")},
            {"preferences.invalid_overlay_color",QT_TRANSLATE_NOOP("Jester","Invalid overlay color")},
            {"preferences.too_many_shortcuts",QT_TRANSLATE_NOOP("Jester","Too many shortcuts")},
            {"preferences.invalid_shortcut",QT_TRANSLATE_NOOP("Jester","Invalid shortcut")},
            {"preferences.shortcut_conflict",QT_TRANSLATE_NOOP("Jester","Conflicting shortcut")},
            {"preferences.unknown_action",QT_TRANSLATE_NOOP("Jester","Unknown editor action")},
            {"preferences.too_many_layouts",QT_TRANSLATE_NOOP("Jester","Too many panel layouts")},
            {"preferences.invalid_layout",QT_TRANSLATE_NOOP("Jester","Invalid panel layout")},
            {"preferences.too_many_scene_presets",QT_TRANSLATE_NOOP("Jester","Too many scene presets")},
            {"preferences.invalid_scene_preset",QT_TRANSLATE_NOOP("Jester","Invalid scene preset")},
            {"preferences.read_failed",QT_TRANSLATE_NOOP("Jester","Cannot read preferences")},
            {"preferences.write_failed",QT_TRANSLATE_NOOP("Jester","Cannot write preferences")},
            {"preferences.file_too_large",QT_TRANSLATE_NOOP("Jester","Preferences file is too large")},
            {"preferences.invalid_json",QT_TRANSLATE_NOOP("Jester","Invalid preferences JSON")},
            {"language.unsupported",QT_TRANSLATE_NOOP("Jester","Unsupported language")},
            {"language.missing_jester_catalog",QT_TRANSLATE_NOOP("Jester","Missing application translation catalog")},
            {"language.missing_qt_catalog",QT_TRANSLATE_NOOP("Jester","Missing Qt translation catalog")}};
        return uiText(labels.value(code,QT_TRANSLATE_NOOP("Jester","Settings error")))+" ["+code+"]"+(detail.isEmpty()?QString():": "+detail);
    }
    if(LocalizationService::instance().language()!="ru")return message;
    const auto direct=uiText(message);if(direct!=message)return direct;
    for(const char *prefix:ownedMessagePrefixes) {
        const auto source=QString::fromUtf8(prefix);
        if(message.startsWith(source))return uiText(source)+message.mid(source.size());
    }
    static QHash<QString,QString> cache;
    if(auto found=cache.constFind(message);found!=cache.cend())return *found;
    struct Pattern {QString source,prefix;QRegularExpression expression;QList<int> arguments;};
    static const auto patterns=[] {
        QList<Pattern> result;const QRegularExpression token("%L?([1-9][0-9]?)");
        for(const char *text:ownedMessagePatterns) {
            Pattern pattern;pattern.source=QString::fromUtf8(text);
            auto matches=token.globalMatch(pattern.source);int previous=0;QString expression="^";
            while(matches.hasNext()) {
                const auto match=matches.next();
                if(pattern.arguments.isEmpty())pattern.prefix=pattern.source.left(match.capturedStart());
                expression+=QRegularExpression::escape(pattern.source.mid(previous,match.capturedStart()-previous))+"(.+?)";
                pattern.arguments<<match.captured(1).toInt();previous=match.capturedEnd();
            }
            expression+=QRegularExpression::escape(pattern.source.mid(previous))+"$";
            pattern.expression=QRegularExpression(expression,QRegularExpression::DotMatchesEverythingOption);result<<pattern;
        }
        return result;
    }();
    QString translated=message;
    for(const auto &pattern:patterns) {
        if(!message.startsWith(pattern.prefix))continue;
        const auto match=pattern.expression.match(message);if(!match.hasMatch())continue;
        QMap<int,QString> arguments;
        for(int i=0;i<pattern.arguments.size();++i)arguments[pattern.arguments[i]]=match.captured(i+1);
        translated=uiText(pattern.source);
        // Replace all placeholders from the end in one pass. Authored argument
        // text containing "%2" must not become another format instruction.
        const QRegularExpression token("%L?([1-9][0-9]?)");auto tokens=token.globalMatch(translated);QList<QRegularExpressionMatch> positions;
        while(tokens.hasNext())positions<<tokens.next();
        for(auto it=positions.crbegin();it!=positions.crend();++it)
            translated.replace(it->capturedStart(),it->capturedLength(),arguments.value(it->captured(1).toInt(),it->captured()));
        break;
    }
    if(cache.size()>=512)cache.clear();cache[message]=translated;return translated;
}
} // namespace trickster
