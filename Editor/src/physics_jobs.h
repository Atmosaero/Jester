#pragma once
#include "workspace.h"
#include <QJsonObject>
#include <thread>
namespace trickster {
// The worker has only a snapshot. Its destination is always an immutable document ID/revision.
class PhysicsJobs {
  public:
    explicit PhysicsJobs(Workspace &workspace) : workspace(workspace) {}
    ~PhysicsJobs();
    bool start(Document &document, bool ballistic, int first, int last, QString &error);
    bool start(Document &document,const QString &operation,int first,int last,const QJsonObject &settings,QString &error);
    void cancel();
    void poll();
    void clear();
    QJsonObject state() const;
    bool hasResult(const Document &document) const;
    std::optional<Scene> result(const Document &document) const;

  private:
    struct Job {
        PhysicsControl control;
        std::atomic<bool> done{false};
        Scene result;
        PhysicsReport report;
        QString documentId, error, status = "running";
        quint64 revision = 0, id = 0;
        bool ok = false;
    };
    Workspace &workspace;
    std::shared_ptr<Job> job;
    std::thread worker;
    quint64 nextId = 0;
    bool valid() const;
};
} // namespace trickster
