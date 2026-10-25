#pragma once
#include <QObject>
#include <QTranslator>
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
QString uiText(const char *source,int count=-1);
} // namespace trickster
