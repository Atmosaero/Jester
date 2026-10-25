#pragma once
#include "settings/preferences.h"
#include <QJsonArray>
#include <QList>
class QAction;
class QWidget;
namespace trickster {
class ActionRegistry {
  public:
    explicit ActionRegistry(QWidget &root);
    QList<QAction*> actions() const {return registered;}
    bool validate(const EditorPreferences &,QString &error) const;
    void apply(const EditorPreferences &) const;
    QJsonArray state() const;
    static QString id(const QAction *);
  private:
    QList<QAction*> registered;
};
}
