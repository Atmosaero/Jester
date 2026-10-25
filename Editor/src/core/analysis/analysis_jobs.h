#pragma once
#include "core/analysis/motion_analysis.h"
#include "core/scene/workspace.h"
#include <thread>
namespace trickster {
class AnalysisJobs {
  public:
    explicit AnalysisJobs(Workspace &w) : workspace(w) {}
    ~AnalysisJobs();
    QJsonObject start(Document &, const QString &, const QJsonObject &, const Document *other = nullptr);
    void poll();
    void cancel(const QString &id);
    QJsonObject state(const QString &id = {}) const;
    QJsonObject result(const QString &id, int offset, int limit) const;
    Scene resultScene(const QString &id, const Document &) const;

  private:
    struct Job {
        QString id, documentId, otherId, operation, status = "running", error;
        quint64 revision = 0, otherRevision = 0, metadataRevision = 0, otherMetadataRevision = 0;
        bool usesContext = false;
        AnalysisControl control;
        std::atomic<bool> done{false};
        QJsonObject report;
        std::optional<Scene> sceneResult;
    };
    Workspace &workspace;
    std::shared_ptr<Job> job;
    std::thread worker;
    bool valid() const;
};
} // namespace trickster
