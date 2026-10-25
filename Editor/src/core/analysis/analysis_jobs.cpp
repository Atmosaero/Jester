#include "core/analysis/analysis_jobs.h"
#include "core/scene/json_helpers.h"
#include "core/animation/retarget.h"
#include <QElapsedTimer>
#include <QUuid>
namespace trickster {
using namespace api_detail;
AnalysisJobs::~AnalysisJobs() {
    if (job)
        job->control.cancel.store(true);
    if (worker.joinable())
        worker.join();
}
bool AnalysisJobs::valid() const {
    if (!job)
        return false;
    auto d = workspace.find(job->documentId);
    auto other = job->otherId.isEmpty() ? nullptr : workspace.find(job->otherId);
    return d && d->motionRevision == job->revision &&
           (!job->usesContext || d->metadataRevision == job->metadataRevision) &&
           (job->otherId.isEmpty() ||
            (other && other->motionRevision == job->otherRevision &&
             (!job->usesContext || other->metadataRevision == job->otherMetadataRevision)));
}
QJsonObject AnalysisJobs::start(Document &d, const QString &operation, const QJsonObject &request,
                                const Document *other) {
    require(!job || job->done.load(), "An analysis is running; cancel and wait before starting another");
    require(operation == "trajectory" || operation == "compare" || operation == "validate" ||
                operation == "retarget",
            "Unknown analysis operation");
    require((operation != "compare" && operation != "retarget") || other,
            "Comparison requires a second document");
    require(!d.inTransaction() && (!other || !other->inTransaction()),
            "Finish the current edit before taking an analysis snapshot");
    double step = request["step"].toDouble(1. / d.scene.fps),
           first = request["start"].toDouble(double(d.scene.first) / d.scene.fps),
           last = request["end"].toDouble(double(d.scene.last) / d.scene.fps);
    require(std::isfinite(step) && step > 0 && std::isfinite(first) && std::isfinite(last) && last >= first &&
                (last - first) / step <= 99999,
            "Invalid or excessive analysis sampling");
    int nodeCount = operation == "trajectory" ? request["nodes"].toArray().size()
                                              : std::max(1, int(request["mapping"].toArray().size()));
    require((std::floor((last - first) / step) + 1) * nodeCount <= 500000,
            "Analysis memory budget exceeded; reduce bones or samples");
    if (worker.joinable())
        worker.join();
    job = std::make_shared<Job>();
    auto j = job;
    j->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    j->documentId = d.id;
    j->revision = d.motionRevision;
    j->metadataRevision = d.metadataRevision;
    j->operation = operation;
    j->usesContext = operation == "validate" || operation == "retarget" ||
                     (operation == "compare" && request["alignment"] == "event");
    if (other) {
        j->otherId = other->id;
        j->otherRevision = other->motionRevision;
        j->otherMetadataRevision = other->metadataRevision;
    }
    Scene snapshot = operation != "trajectory" ? d.scene : d.scene.motionOnlyCopy(),
          second = other ? (operation == "compare" ? other->scene : other->scene.motionOnlyCopy()) : Scene{};
    if (operation == "compare" && request["mapping"].toArray().isEmpty())
        require((std::floor((last - first) / step) + 1) * 128 <= 500000,
                "Comparison memory budget exceeded; provide an explicit smaller mapping");
    worker = std::thread([j, snapshot = std::move(snapshot), second = std::move(second), request] {
        try {
            if (j->operation == "retarget") {
                QJsonObject report;
                j->sceneResult = retargetMotion(second, snapshot, request, report, &j->control);
                j->report = report;
                j->done.store(true);
                return;
            }
            QElapsedTimer elapsed;
            elapsed.start();
            int budget = std::clamp(request["timeBudgetMs"].toInt(30000), 1, 120000), offset = 0;
            QJsonArray rows;
            auto plannedRequest = request;
            if (j->operation == "validate" && request["adaptive"].toBool())
                plannedRequest["sampleTimes"] = validationSampleTimes(snapshot, request, &j->control);
            QJsonObject page;
            do {
                auto args = plannedRequest;
                args["offset"] = offset;
                args["limit"] = 128;
                args["budgetMs"] = request["operation"] == "lag" ? 1000 : 100;
                page = j->operation == "trajectory" ? motionTrajectory(snapshot, args, &j->control)
                       : j->operation == "validate" ? validateMotion(snapshot, args, &j->control)
                                                    : compareMotion(snapshot, second, args, &j->control);
                for (auto row : page["samples"].toArray())
                    rows.append(row);
                offset = page["nextOffset"].toInt(-1);
            } while (offset >= 0 && !j->control.cancel.load() && elapsed.elapsed() < budget);
            if (j->operation == "trajectory") {
                QMap<int, double> length;
                bool undefined = false;
                for (auto value : rows)
                    for (auto item : value.toObject()["bones"].toArray()) {
                        auto bone = item.toObject();
                        auto distance = bone["distanceFromPrevious"];
                        if (distance.isDouble())
                            length[bone["node"].toInt()] += distance.toDouble();
                        else
                            undefined = true;
                    }
                QJsonArray totals;
                for (auto it = length.begin(); it != length.end(); ++it)
                    totals.append(QJsonObject{{"node", it.key()}, {"length", it.value()}});
                page["pathLengths"] = totals;
                page["pathLengthMeaning"] = "Sampled polyline length in positionUnits and the chosen moving "
                                            "reference; discontinuities are omitted, not bridged";
                page["pathLengthHasUndefinedIntervals"] = undefined;
            }
            if (j->operation == "compare" && request["operation"] != "lag") {
                double p2 = 0, a2 = 0, peak = 0;
                int count = 0;
                for (auto value : rows)
                    for (auto bone : value.toObject()["bones"].toArray()) {
                        auto b = bone.toObject();
                        double p = b["positionError"].toDouble(),
                               angle = b["orientationErrorDegrees"].toDouble();
                        p2 += p * p;
                        a2 += angle * angle;
                        peak = std::max(peak, p);
                        ++count;
                    }
                page["rmsPosition"] = count ? std::sqrt(p2 / count) : 0;
                page["peakPosition"] = peak;
                page["rmsOrientationDegrees"] = count ? std::sqrt(a2 / count) : 0;
            }
            if (j->operation == "validate") {
                QMap<QString, QJsonObject> active;
                QJsonArray problems;
                int serial = 0;
                for (auto value : rows) {
                    auto row = value.toObject();
                    double time = row["seconds"].toDouble();
                    QSet<QString> present;
                    for (auto entry : row["issues"].toArray()) {
                        auto issue = entry.toObject();
                        auto base = issue["id"].toString() + ":" + issue["status"].toString() + ":" +
                                    issue["allowedBy"].toString();
                        present.insert(base);
                        if (!active.contains(base)) {
                            issue["problemId"] = j->id + ":" + QString::number(++serial);
                            issue["firstSeconds"] = time;
                            issue["lastSeconds"] = time;
                            issue["peakSeconds"] = time;
                            issue["samples"] = 0;
                            active[base] = issue;
                        }
                        auto &problem = active[base];
                        problem["lastSeconds"] = time;
                        problem["samples"] = problem["samples"].toInt() + 1;
                        double peak = issue.contains("angleDegrees") ? issue["angleDegrees"].toDouble()
                                                                     : issue["depthMetres"].toDouble();
                        double previous = problem.contains("angleDegrees")
                                              ? problem["angleDegrees"].toDouble()
                                              : problem["depthMetres"].toDouble();
                        if (peak > previous) {
                            for (auto field = issue.begin(); field != issue.end(); ++field)
                                problem[field.key()] = field.value();
                            problem[issue.contains("angleDegrees") ? "angleDegrees" : "depthMetres"] = peak;
                            problem["peakSeconds"] = time;
                        }
                    }
                    for (auto key : active.keys())
                        if (!present.contains(key)) {
                            problems.append(active.take(key));
                        }
                }
                for (auto problem : active)
                    problems.append(problem);
                page["problems"] = problems;
                page["problemIntervalsMeaning"] =
                    "First and last sampled detections; events between samples are not certified. Problem "
                    "IDs remain stable for this immutable job.";
            }
            page["samples"] = rows;
            page["elapsedMs"] = double(elapsed.elapsed());
            const bool incompleteAlgorithm = page["status"] == "budget-exhausted";
            page["status"] = j->control.cancel.load()             ? "cancelled"
                             : offset >= 0 || incompleteAlgorithm ? "budget-exhausted"
                                                                  : "complete";
            page["summaryScope"] = "all returned samples";
            j->report = page;
        } catch (const std::exception &e) {
            j->error = QString::fromUtf8(e.what());
        }
        j->done.store(true);
    });
    return state(j->id);
}
void AnalysisJobs::poll() {
    if (!job)
        return;
    if (!valid())
        job->control.cancel.store(true);
    if (job->status != "running" || !job->done.load())
        return;
    if (worker.joinable())
        worker.join();
    job->status = !valid()                     ? "stale"
                  : job->control.cancel.load() ? "cancelled"
                  : !job->error.isEmpty()      ? "failed"
                                               : "ready";
    if (job->status == "ready")
        job->control.progress.store(100);
}
void AnalysisJobs::cancel(const QString &id) {
    require(job && job->id == id, "Unknown analysis job");
    job->control.cancel.store(true);
}
QJsonObject AnalysisJobs::state(const QString &id) const {
    if (!job) {
        require(id.isEmpty(), "Unknown analysis job");
        return {{"status", "idle"}};
    }
    require(id.isEmpty() || id == job->id, "Unknown analysis job");
    return {{"id", job->id},
            {"operation", job->operation},
            {"documentId", job->documentId},
            {"motionRevision", qint64(job->revision)},
            {"contextRevision", qint64(job->metadataRevision)},
            {"otherDocumentId", job->otherId},
            {"otherMotionRevision", qint64(job->otherRevision)},
            {"otherContextRevision", qint64(job->otherMetadataRevision)},
            {"dependsOnContext", job->usesContext},
            {"status", valid() ? job->status : QString("stale")},
            {"done", job->done.load()},
            {"progress", job->control.progress.load()},
            {"cancelRequested", job->control.cancel.load()},
            {"error", job->done.load() ? job->error : QString()}};
}
QJsonObject AnalysisJobs::result(const QString &id, int offset, int limit) const {
    require(job && job->id == id, "Unknown analysis job");
    require(valid(), "Analysis snapshot is stale");
    require(job->status == "ready", "Analysis is not ready");
    require(offset >= 0 && limit > 0 && limit <= 2000, "Invalid result pagination");
    auto result = job->report;
    auto rows = result["samples"].toArray();
    require(offset <= std::max(rows.size(), result["problems"].toArray().size()),
            "Result offset out of range");
    QJsonArray page;
    for (int i = offset; i < std::min(int(rows.size()), offset + limit); ++i)
        page.append(rows[i]);
    result["samples"] = page;
    if (result.contains("problems")) {
        auto all = result["problems"].toArray();
        QJsonArray problems;
        for (int i = offset; i < std::min(int(all.size()), offset + limit); ++i)
            problems.append(all[i]);
        result["problems"] = problems;
        result["totalProblems"] = all.size();
        result["nextProblemOffset"] = offset + problems.size() < all.size() ? offset + problems.size() : -1;
    }
    result["nextResultOffset"] = offset + page.size() < rows.size() ? offset + page.size() : -1;
    result["returnedSampleCount"] = rows.size();
    result["documentId"] = job->documentId;
    result["motionRevision"] = qint64(job->revision);
    result["jobId"] = job->id;
    return result;
}
Scene AnalysisJobs::resultScene(const QString &id, const Document &target) const {
    require(job && job->id == id && job->documentId == target.id && valid() && job->status == "ready" &&
                job->sceneResult.has_value(),
            "Retarget result is unavailable, belongs to another document, or is stale");
    return *job->sceneResult;
}
} // namespace trickster
