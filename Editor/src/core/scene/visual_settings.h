#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QVector>

namespace trickster {
enum class VisualFieldKind { Toggle, Number, Color, Choice };
struct VisualField {
    QString path;
    const char *group;
    const char *label;
    VisualFieldKind kind;
    double minimum = 0, maximum = 1, step = .1;
    QStringList choices;
};

// Graphics-independent authored settings. GUI, project IO and MCP share this
// complete model and its validation; no Window or SDK types belong here.
class SceneVisualSettings {
  public:
    SceneVisualSettings();
    QJsonObject toJson() const { return values; }
    QJsonValue value(const QString &path) const;
    bool patch(const QJsonObject &changes, QString &error);
    bool setValue(const QString &path, const QJsonValue &value, QString &error);
    static bool fromJson(const QJsonObject &, SceneVisualSettings &, QString &error);
    static SceneVisualSettings preset(const QString &name);
    static const QVector<VisualField> &fields();
    static QJsonObject schema();
    bool operator==(const SceneVisualSettings &other) const { return values == other.values; }
    bool operator!=(const SceneVisualSettings &other) const { return !(*this == other); }
  private:
    QJsonObject values;
};
} // namespace trickster
