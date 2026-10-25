#include "physics/physics_jobs.h"
#include "physics/physics_tools.h"
#include <QCryptographicHash>
#include <QJsonDocument>
namespace trickster {
PhysicsJobs::~PhysicsJobs() {
    cancel();
    if (worker.joinable())
        worker.join();
}
bool PhysicsJobs::valid() const {
    if (!job)
        return false;
    auto d = workspace.find(job->documentId);
    return d && d->motionRevision == job->revision;
}
void PhysicsJobs::cancel() {
    if (job && job->status == "running")
        job->control.cancel.store(true);
}
void PhysicsJobs::clear() {
    cancel();
    if (job && job->done.load())
        job->status = "cleared";
}
QJsonObject PhysicsJobs::state() const {
    if (!job)
        return {{"status", "idle"}};
    auto status = valid() ? job->status : QString("stale");
    return {{"id", qint64(job->id)},
            {"documentId", job->documentId},
            {"revision", qint64(job->revision)},
            {"revisionKind", "motion"},
            {"status", status},
            {"progress", job->control.progress.load()},
            {"done", job->done.load()},
            {"cancelRequested", job->control.cancel.load()},
            { "report", status == "ready" ? job->report.json() : QJsonObject{}},
            {"summary", status == "ready" ? job->report.summary() : QString()},
            {"error", job->done.load() ? job->error : QString()}};
}
bool PhysicsJobs::start(Document &d,bool ballistic,int first,int last,QString &error) {
 return start(d,ballistic?"flight":"simulate",first,last,{},error);
}
bool PhysicsJobs::start(Document &d,const QString &operation,int first,int last,const QJsonObject &settings,QString &error) {
    if (job && !job->done.load()) {
        error = "A physics calculation is running. Cancel it first.";
        return false;
    }
    if (!d.scene.validate(&error))
        return false;
    if (d.scene.nodes.empty() || first < d.scene.first || last > d.scene.last || first >= last) {
        error = "Choose at least two frames inside the clip.";
        return false;
    }
    if (worker.joinable())
        worker.join();
    job = std::make_shared<Job>();
    auto j = job;
    j->documentId = d.id;
    j->revision = d.motionRevision;
    j->id = ++nextId;
    auto requestSettings=settings.isEmpty()?d.scene.physics.requests[operation].toObject():settings;
    Scene snapshot = d.scene;
    snapshot.compareOriginal = false;
    worker = std::thread([j, snapshot = std::move(snapshot), first, last, operation, settings=requestSettings] {
        try {
            if(operation=="flight")j->ok=solveFlight(snapshot,first,last,flightSettings(settings),j->result,j->report,j->error,&j->control);
            else if(operation=="optimize")j->ok=optimizeDynamics(snapshot,first,last,dynamicsSettings(settings),j->result,j->report,j->error,&j->control);
            else if(operation=="simulate")j->ok=simulatePhysics(snapshot,first,last,j->result,j->report,j->error,&j->control);
            else j->error="Unknown physics operation";
        } catch (const std::exception &e) {
            j->error = QString::fromUtf8(e.what());
        }
        if(j->ok) {
            j->report.details["sourceDocumentId"]=j->documentId;j->report.details["sourceRevision"]=qint64(j->revision);j->report.details["sourceRevisionKind"]="motion";
            j->report.details["inputSha256"]=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(sceneToJson(snapshot)).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
            j->report.details["requestSettings"]=settings;j->result.physicsResult=j->report.json();
        }
        j->done.store(true);
    });
    return true;
}
void PhysicsJobs::poll() {
    if (!job || job->status != "running")
        return;
    if (!valid())
        job->control.cancel.store(true);
    if (!job->done.load())
        return;
    if (worker.joinable())
        worker.join();
    job->status = !valid()                     ? "stale"
                  : job->control.cancel.load() ? "cancelled"
                  : !job->ok                   ? "failed"
                                               : "ready";
    if (job->status == "ready")
        job->control.progress.store(100);
}
bool PhysicsJobs::hasResult(const Document &d) const {
    return job && valid() && job->status == "ready" && job->documentId == d.id;
}
std::optional<Scene> PhysicsJobs::result(const Document &d) const {
    if (!hasResult(d))
        return {};
    return job->result;
}
} // namespace trickster
