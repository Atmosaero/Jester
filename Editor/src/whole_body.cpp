#include "runtime_error.h"
#include "mujoco_backend.h"
#include "physics_provenance.h"
#include "physics_tools.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace trickster {
namespace {
void need(bool ok, const QString &e) {
    if (!ok)
        throw trickster::RuntimeError(e.toStdString());
}
QJsonArray vec(QVector3D p) {
    return {p.x(), p.y(), p.z()};
}
} // namespace
namespace {
std::vector<EffectorGoal> allGoals(const Scene &s, const WholeBodySettings &settings) {
    auto goals = settings.goals;
    if (settings.useContacts) {
        for (auto it = s.pins.begin(); it != s.pins.end(); ++it)
            if (it->weight > 0)
                goals.push_back({it.key(), 3, it->target, it->weight, it->weight >= .999f});
        for (const auto &c : s.contacts) {
            float w = contactWeight(c, s.sampleFrame);
            if (w > 0)
                goals.push_back({c.node, 3, c.target, w, w >= .999f, c.orient, c.orientation});
        }
    }
    return goals;
}
} // namespace
void verifyWholeBodyResult(const Scene &s, const WholeBodySettings &settings, QJsonObject &report) {
    auto p = s.positions(s.pose);
    auto goals = allGoals(s, settings);
    QJsonArray residuals;
    double hard = 0, soft = 0, angleError = 0;
    int orientations = 0;
    double H = std::max(.01f, s.height() * s.physics.metersPerUnit);
    for (const auto &g : goals) {
        double e = (p[g.node] - g.target).length() * s.physics.metersPerUnit;
        if (g.hard)
            hard = std::max(hard, e);
        else
            soft = std::max(soft, e);
        QJsonObject r{{"node", g.node},
                      {"hard", g.hard},
                      {"priority", g.priority},
                      {"weight", g.weight},
                      {"residualM", e}};
        if (g.orient) {
            auto d = (g.orientation * s.globalRotation(g.node, s.pose).conjugated()).normalized();
            double angle = qRadiansToDegrees(2 * std::acos(std::clamp(double(std::abs(d.scalar())), 0., 1.)));
            angleError = std::max(angleError, angle);
            r["orientationResidualDeg"] = angle;
            ++orientations;
        }
        residuals.append(r);
    }
    auto com = physicalCenterOfMass(s);
    auto delta = (com - settings.comTarget) * s.physics.metersPerUnit;
    if (!settings.comHeight)
        delta.setY(0);
    double comError = settings.comEnabled ? delta.length() : 0;
    bool reached = hard <= .002 * H && soft <= .005 * H && comError <= .005 * H && angleError <= 5;
    report["status"] = reached ? "solved" : "infeasible";
    report["stopReason"] =
        reached ? "Goals reached within editing tolerances."
                : "Goal/limit/priority or authored-contact conflict; relax the reported target/support.";
    report["contactErrorM"] = hard;
    report["targetErrorM"] = soft;
    report["targetErrorDeg"] = orientations ? QJsonValue(angleError) : QJsonValue(QJsonValue::Null);
    report["contactAngleErrorDeg"] = orientations ? QJsonValue(angleError) : QJsonValue(QJsonValue::Null);
    report["effectors"] = residuals;
    report["comEnabled"] = settings.comEnabled;
    report["comTargetM"] = settings.comEnabled ? QJsonValue(vec(settings.comTarget * s.physics.metersPerUnit))
                                               : QJsonValue(QJsonValue::Null);
    report["comAchievedM"] = vec(com * s.physics.metersPerUnit);
    report["comErrorM"] = settings.comEnabled ? QJsonValue(comError) : QJsonValue(QJsonValue::Null);
    report["staticHintOnly"] = true;
    report["dynamicValidation"] = "not performed";
}
bool solveWholeBody(Scene &s, const WholeBodySettings &settings, PhysicsReport &report, QString &error,
                    PhysicsControl *control) {
    try {
        QElapsedTimer timer;
        timer.start();
        Scene initialScene = s;
        need(settings.iterations > 0 && settings.iterations <= 1000,
             "Whole-body iteration budget must be 1–1000.");
        need(std::isfinite(settings.poseWeight) && settings.poseWeight > 0,
             "Pose regularization must be positive.");
        mj::Backend b;
        need(b.compile(s, error, false), error);
        auto m = b.model.get();
        auto d = b.data();
        need(b.pose(s, d->qpos, error), error);
        std::vector<double> initial(d->qpos, d->qpos + m->nq);
        auto goals = allGoals(s, settings);
        need(!goals.empty() || settings.comEnabled, "Supply an effector or COM goal.");
        for (const auto &g : goals)
            need(b.indices.contains(g.node) && std::isfinite(g.target.lengthSquared()) &&
                     std::isfinite(g.weight) && g.weight > 0 && g.priority >= 0 && g.priority <= 3 &&
                     (!g.orient || (std::isfinite(g.orientation.lengthSquared()) &&
                                    g.orientation.lengthSquared() > 1e-10)),
                 "Invalid whole-body effector/weight/priority.");
        need(std::isfinite(settings.comWeight) && settings.comWeight > 0, "Invalid COM weight.");
        need(!settings.comEnabled || std::isfinite(settings.comTarget.lengthSquared()), "Invalid COM goal.");
        int nv = m->nv;
        double H = std::max(.01f, s.height() * b.scale);
        std::vector<double> A(nv * nv), rhs(nv), step(nv), jp(3 * nv), jr(3 * nv), prior(nv);
        report = {};
        report.backend = physicsBackendVersion();
        for (int iteration = 0; iteration < settings.iterations; ++iteration) {
            need(!control || !control->cancel.load(), "Whole-body solve cancelled.");
            if (control)
                control->progress.store(iteration * 100 / settings.iterations);
            mj_forward(m, d.get());
            std::fill(A.begin(), A.end(), 0);
            std::fill(rhs.begin(), rhs.end(), 0);
            auto row = [&](const double *jac, double residual, double weight) {
                for (int i = 0; i < nv; ++i) {
                    rhs[i] += weight * jac[i] * residual;
                    for (int j = 0; j < nv; ++j)
                        A[i * nv + j] += weight * jac[i] * jac[j];
                }
            };
            for (const auto &g : goals) {
                int site = b.bodies[b.indices[g.node]].site;
                mj_jacSite(m, d.get(), jp.data(), jr.data(), site);
                double weight = g.weight * (g.hard ? 1e5 : std::pow(10., g.priority));
                for (int k = 0; k < 3; ++k)
                    row(jp.data() + k * nv, g.target[k] * b.scale - d->site_xpos[3 * site + k], weight);
                if (g.orient) {
                    int body = m->site_bodyid[site];
                    QQuaternion current(float(d->xquat[4 * body]), float(d->xquat[4 * body + 1]),
                                        float(d->xquat[4 * body + 2]), float(d->xquat[4 * body + 3]));
                    auto delta = (g.orientation * current.conjugated()).normalized();
                    if (delta.scalar() < 0)
                        delta = -delta;
                    QVector3D axis;
                    float angle;
                    delta.getAxisAndAngle(&axis, &angle);
                    auto error = axis * qDegreesToRadians(angle);
                    for (int k = 0; k < 3; ++k)
                        row(jr.data() + k * nv, error[k], weight * H * H * .1);
                }
            }
            if (settings.comEnabled) {
                mj_jacSubtreeCom(m, d.get(), jp.data(), 0);
                for (int k = 0; k < 3; ++k)
                    if (k != 1 || settings.comHeight)
                        row(jp.data() + k * nv, settings.comTarget[k] * b.scale - d->subtree_com[k],
                            settings.comWeight * 10);
            }
            mj_differentiatePos(m, prior.data(), 1, d->qpos, initial.data());
            for (int i = 0; i < nv; ++i) {
                A[i * nv + i] += settings.poseWeight + .00001;
                rhs[i] += settings.poseWeight * prior[i];
            }
            mju_cholFactor(A.data(), nv, 1e-12);
            mju_cholSolve(step.data(), A.data(), rhs.data(), nv);
            double maxStep = 0;
            for (int i = 0; i < nv; ++i)
                maxStep = std::max(maxStep, std::abs(step[i]));
            double factor = maxStep > .18 ? .18 / maxStep : 1;
            mj_integratePos(m, d->qpos, step.data(), factor);
            for (int j = 0; j < m->njnt; ++j)
                if (m->jnt_type[j] == mjJNT_HINGE && m->jnt_limited[j]) {
                    int a = m->jnt_qposadr[j];
                    d->qpos[a] = std::clamp(d->qpos[a], m->jnt_range[2 * j], m->jnt_range[2 * j + 1]);
                }
            mj_forward(m, d.get());
            b.reconstruct(s, d.get());
            s.enforceLimits();
            need(b.pose(s, d->qpos, error), error);
            report.iterations = iteration + 1;
            if (maxStep < 1e-7)
                break;
        }
        mj_forward(m, d.get());
        b.reconstruct(s, d.get());
        s.enforceLimits();
        report.dofs = nv;
        report.frames = 1;
        report.bodies = int(b.bodies.size());
        report.totalMass = physicalMass(s);
        report.elapsedMs = timer.elapsed();
        report.maxLimitError = b.limitError(s);
        auto verified = report.json();
        verifyWholeBodyResult(s, settings, verified);
        report.details = verified;
        report.status = verified["status"].toString();
        report.stopReason = verified["stopReason"].toString();
        report.maxContactError = float(verified["contactErrorM"].toDouble());
        report.maxTargetPosition = float(verified["targetErrorM"].toDouble());
        report.maxTargetAngle = float(verified["targetErrorDeg"].toDouble());
        report.maxContactAngle = report.maxTargetAngle;
        report.warnings << "COM projection is a static hint; this pose has not been dynamically validated.";
        recordPhysicsProvenance(initialScene, int(s.sampleFrame), int(s.sampleFrame), report, b.xml);
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
} // namespace trickster
