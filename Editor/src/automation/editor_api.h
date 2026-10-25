#pragma once
#include "core/scene/core.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <optional>
namespace trickster {
class Window;
class EditorApi : public QObject {
    Q_OBJECT
  public:
    explicit EditorApi(Window *window, QObject *parent = nullptr);
    QJsonArray tools() const;
    QJsonArray resources() const;
    QJsonObject readResource(const QString &uri) const;
    QJsonObject call(const QString &name, const QJsonObject &arguments);
    QJsonObject state(bool nodes = true) const;
    void nameActions();
    void cancelInput();
    std::function<bool()> requestCancelled;
    QJsonArray actions() const;
    QJsonArray controls() const;
    static QJsonObject textResult(const QJsonObject &value, bool error = false);

  private:
    Window *w;
    QJsonArray catalog;
    std::optional<Key> clipboard;
    mutable int nextUiId = 1;
    int revision = 0;
    QJsonObject execute(const QString &name, const QJsonObject &args);
    QJsonObject uiCall(const QString &name, const QJsonObject &args);
    QString uiId(QObject *object) const;
    QObject *findUi(const QString &id) const;
};
} // namespace trickster
