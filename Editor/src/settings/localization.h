#pragma once
#include <QObject>
#include <QTranslator>
#include <QCoreApplication>
namespace trickster {
class LocalizationService final:public QObject {
    Q_OBJECT
  public:
    static LocalizationService &instance();
    bool setPreference(const QString &language,QString &error);
    QString preference() const {return requested;}
    QString language() const {return active;}
    bool qtTranslationLoaded() const {return qtLoaded;}
    static QString systemLanguage();
  signals:
    void languageChanged();
  private:
    explicit LocalizationService(QObject *parent);
    QString requested="en",active="en";
    QTranslator own,qt;
    bool qtLoaded=false,initialized=false;
};
inline QString uiText(const char *source,int count=-1) {return QCoreApplication::translate("Jester",source,nullptr,count);}
inline QString uiText(const QString &source) {const auto utf8=source.toUtf8();return uiText(utf8.constData());}
QString translatedDiagnostic(const QString &message);
QString ownedValueLabel(const QString &value);
} // namespace trickster
