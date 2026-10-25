#pragma once
#include "core/scene/workspace.h"
#include <QTimer>
namespace trickster {
class RecoveryStore : public QObject {
    Q_OBJECT
  public:
    explicit RecoveryStore(Workspace &workspace, QString directory = {}, QObject *parent = nullptr);
    QString directory() const {
        return folder;
    }
    QStringList snapshots() const;
    bool flush(QString &error);
    bool recover(const QString &file, QString &error);
    void discard(const QString &documentId);
    void start(int intervalMs = 30000) {
        timer.start(intervalMs);
    }
  signals:
    void failed(QString error);

  private:
    Workspace &workspace;
    QString folder;
    QTimer timer;
    QMap<QString, quint64> saved;
    QMap<QString, QString> recoveredFiles;
};
} // namespace trickster
