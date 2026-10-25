#pragma once
#include "settings/preferences.h"
#include <QObject>
namespace trickster {
class PreferencesService final:public QObject {
    Q_OBJECT
  public:
    static PreferencesService &instance();
    const EditorPreferences &current() const {return preferences;}
    const QString &loadWarning() const {return warning;}
    const QString &storagePath() const {return path;}
    bool preview(const EditorPreferences &,QString &error);
    bool apply(const EditorPreferences &,QString &error);
  signals:
    void changed();
  private:
    explicit PreferencesService(QObject *parent);
    EditorPreferences preferences;
    QString path,warning;
};
} // namespace trickster
