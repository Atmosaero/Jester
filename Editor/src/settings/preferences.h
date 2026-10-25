#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace trickster {
// Only portable editor preferences belong here: no document paths, recent
// files, MCP endpoints or credentials. Runtime storage location is external.
class EditorPreferences {
  public:
    EditorPreferences();
    QJsonObject toJson() const { return values; }
    QJsonValue value(const QString &path) const;
    bool patch(const QJsonObject &, QString &error);
    static bool fromJson(const QJsonObject &, EditorPreferences &, QString &error);
    static bool load(const QString &path, EditorPreferences &, QString &error);
    bool save(const QString &path, QString &error) const;
    bool operator==(const EditorPreferences &other) const { return values==other.values; }
    bool operator!=(const EditorPreferences &other) const { return !(*this==other); }
  private:
    QJsonObject values;
};
} // namespace trickster
