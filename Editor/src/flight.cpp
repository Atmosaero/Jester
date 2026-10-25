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
QJsonArray vec(QVector3D v) {
    return {v.x(), v.y(), v.z()};
}
void rootTranslate(Scene &s, QVector3D delta) {
    for (int i = 0; i < int(s.nodes.size()); ++i)
        if (s.nodes[i].parent < 0)
            s.pose[i].t += delta;
}
} // namespace
bool solveFlight(const Scene &source, int first, int last, const FlightSettings &settings, Scene &result,
                 PhysicsReport &report, QString &error, PhysicsControl *control) {
    try {
        QElapsedTimer timer;
        timer.start();
        need(settings.outputFps >= 0 && settings.outputFps <= 240, "Invalid flight output FPS.");
        need(first >= source.first && last <= source.last && last > first,
             "Choose a flight interval of two or more frames.");
        need(qint64(source.last - source.first + 1) * source.nodes.size() <= 5000000,
             "Range too large to bake.");
        for (const auto &c : physicsContactSchedule(source))
            need(c.last <= first || c.first >= last,
                 "Flight overlaps a planted contact. Move takeoff/landing boundaries.");
        mj::Backend b;
        need(b.compile(source, error, false), error);
        auto m = b.model.get();
        auto d = b.data();
        Scene sample = source, next = source;
        sample.compareOriginal = false;
        next.compareOriginal = false;
        sample.evaluate(first);
        need(sample.pins.isEmpty(), "Release world pins throughout free flight.");
        auto start = physicalCenterOfMass(sample) * b.scale;
        sample.evaluate(last);
        auto end = physicalCenterOfMass(sample) * b.scale;
        double duration = double(last - first) / source.fps;
        QVector3D gravity(0, -source.physics.gravity, 0);
        auto velocity = settings.initialVelocityMode
                            ? settings.initialVelocity
                            : (end - start) / float(duration) - gravity * float(.5 * duration);
        need(std::isfinite(velocity.lengthSquared()) && std::isfinite(settings.endVelocity.lengthSquared()),
             "Invalid flight velocity.");
        auto calculatedEndVelocity = velocity + gravity * float(duration);
        double H = std::max(.01f, source.height() * b.scale),
               tol = settings.velocityTolerance < 0
                         ? .01 * std::sqrt(std::max(1.f, source.physics.gravity) * H)
                         : settings.velocityTolerance;
        double velocityError = (calculatedEndVelocity - settings.endVelocity).length();
        need(!settings.constrainEndVelocity || velocityError <= tol,
             QString("Incompatible end velocity: residual %1 m/s (tolerance %2). Release end velocity, "
                     "change duration, or release endpoint position.")
                 .arg(velocityError)
                 .arg(tol));
        std::vector<int> roots;
        for (int j = 0; j < m->njnt; ++j)
            if (m->jnt_type[j] == mjJNT_FREE)
                roots.push_back(j);
        need(!settings.conserveAngularMomentum || roots.size() == 1,
             "Angular flight correction requires one connected physical root.");
        sample.evaluate(first);
        need(b.pose(sample, d->qpos, error), error);
        std::vector<double> q0(m->nq), q1(m->nq), authVel(m->nv), Hmat(3 * m->nv), integrate(m->nv);
        next.evaluate(std::min(first + 1, last));
        need(b.pose(next, q1.data(), error), error);
        mj_differentiatePos(m, d->qvel, 1. / source.fps, d->qpos, q1.data());
        auto initial = b.measure(d.get());
        auto momentum = initial.angularMomentum;
        int root = roots.empty() ? -1 : roots.front(), qa = root < 0 ? 0 : m->jnt_qposadr[root],
            va = root < 0 ? 0 : m->jnt_dofadr[root];
        std::vector<double> orientation(4);
        if (root >= 0)
            std::copy(d->qpos + qa + 3, d->qpos + qa + 7, orientation.begin());
        int ratio = settings.outputFps == 0 ? std::max(1, 240 / source.fps) : settings.outputFps / source.fps;
        need(ratio >= 1 && ratio * source.fps <= 240 &&
                 (settings.outputFps == 0 || settings.outputFps == ratio * source.fps),
             "Flight output FPS must be a source-rate multiple up to 240.");
        Scene out = physicsBake(source, control, ratio * source.fps);
        out.bodies = mj::definitions(source);
        report = {};
        report.ballistic = true;
        report.totalMass = physicalMass(source);
        report.frames = (last - first) * ratio + 1;
        report.bodies = int(b.bodies.size());
        report.dofs = m->nv;
        int substeps = std::max(4, source.physics.substeps / ratio);
        double dt = 1. / (out.fps * substeps);
        QJsonArray trajectories;
        double sourceBallisticError = 0;
        for (int f = first * ratio; f <= last * ratio; ++f) {
            need(!control || !control->cancel.load(), "Flight correction cancelled.");
            if (control)
                control->progress.store(100 * (f - first * ratio) / std::max(1, (last - first) * ratio));
            if (f > first * ratio && settings.conserveAngularMomentum)
                for (int sub = 0; sub < substeps; ++sub) {
                    float t = (float(f - 1) + float(sub + .5f) / substeps) / ratio;
                    sample.evaluate(t);
                    next.evaluate(std::min(float(last), t + .25f / (substeps * ratio)));
                    need(sample.pins.isEmpty() && next.pins.isEmpty(),
                         "A keyed world pin is active inside free flight.");
                    need(b.pose(sample, q0.data(), error) && b.pose(next, q1.data(), error), error);
                    mj_differentiatePos(m, authVel.data(), dt * .25, q0.data(), q1.data());
                    mju_copy(d->qpos, q0.data(), m->nq);
                    std::copy(orientation.begin(), orientation.end(), d->qpos + qa + 3);
                    mju_copy(d->qvel, authVel.data(), m->nv);
                    for (int k = 0; k < 6; ++k)
                        d->qvel[va + k] = 0;
                    mj_forward(m, d.get());
                    mj_angmomMat(m, d.get(), Hmat.data(), 0);
                    double needL[3] = {momentum.x(), momentum.y(), momentum.z()};
                    for (int k = 0; k < 3; ++k)
                        for (int j = 0; j < m->nv; ++j)
                            needL[k] -= Hmat[k * m->nv + j] * d->qvel[j];
                    double A[9]{}, rhs[3]{}, omega[3]{};
                    for (int i = 0; i < 3; ++i)
                        for (int k = 0; k < 3; ++k) {
                            rhs[i] += Hmat[k * m->nv + va + 3 + i] * needL[k];
                            for (int j = 0; j < 3; ++j)
                                A[3 * i + j] += Hmat[k * m->nv + va + 3 + i] * Hmat[k * m->nv + va + 3 + j];
                        }
                    need(mju_cholFactor(A, 3, 1e-16) == 3, "Degenerate angular inertia.");
                    mju_cholSolve(omega, A, rhs, 3);
                    std::fill(integrate.begin(), integrate.end(), 0);
                    for (int k = 0; k < 3; ++k)
                        integrate[va + 3 + k] = omega[k];
                    mj_integratePos(m, d->qpos, integrate.data(), dt);
                    std::copy(d->qpos + qa + 3, d->qpos + qa + 7, orientation.begin());
                }
            sample.evaluate(float(f) / ratio);
            need(sample.pins.isEmpty(), "A world pin is active inside free flight.");
            out.evaluate(f);
            if (settings.conserveAngularMomentum) {
                need(b.pose(sample, d->qpos, error), error);
                std::copy(orientation.begin(), orientation.end(), d->qpos + qa + 3);
                mj_forward(m, d.get());
                b.reconstruct(out, d.get());
            }
            double t = double(f - first * ratio) / out.fps;
            auto desired = start + velocity * float(t) + gravity * float(.5 * t * t);
            sourceBallisticError = std::max(
                sourceBallisticError, double((physicalCenterOfMass(sample) * b.scale - desired).length()));
            auto before = physicalCenterOfMass(out) * b.scale;
            rootTranslate(out, (desired - before) / b.scale);
            out.keys[f] = {out.pose, {}, out.morphs};
            report.maxLimitError = std::max(report.maxLimitError, b.limitError(out));
            report.maxBallisticError =
                std::max(report.maxBallisticError, (physicalCenterOfMass(out) * b.scale - desired).length());
            QJsonObject sourceEffectors, resultEffectors;
            auto ps = sample.positions(sample.pose), pr = out.positions(out.pose);
            for (const auto &body : b.bodies) {
                auto id = QString::number(body.definition.node);
                sourceEffectors[id] = vec(ps[body.definition.node] * b.scale);
                resultEffectors[id] = vec(pr[body.definition.node] * b.scale);
            }
            trajectories.append(QJsonObject{{"frame", f},
                                            {"sourceComM", vec(physicalCenterOfMass(sample) * b.scale)},
                                            {"resultComM", vec(physicalCenterOfMass(out) * b.scale)},
                                            {"sourceEffectorsM", sourceEffectors},
                                            {"resultEffectorsM", resultEffectors}});
        }
        double momentumScale = report.totalMass * H * std::sqrt(std::max(1.f, source.physics.gravity) * H);
        double denom = std::max(double(momentum.length()), .01 * momentumScale), maxDrift = 0,
               sourceDrift = 0;
        Scene sourcePrevious = source.motionOnlyCopy(), sourceNext = source.motionOnlyCopy(),
              sourceCenter = source.motionOnlyCopy();
        if (settings.conserveAngularMomentum)
            for (int f = first * ratio + 1; f < last * ratio; ++f) {
                sample = out;
                next = out;
                sample.evaluate(f - .5f);
                next.evaluate(f + .5f);
                need(b.pose(sample, q0.data(), error) && b.pose(next, q1.data(), error), error);
                mj_differentiatePos(m, d->qvel, 1. / out.fps, q0.data(), q1.data());
                sample.evaluate(f);
                need(b.pose(sample, d->qpos, error), error);
                auto measurement = b.measure(d.get());
                maxDrift =
                    std::max(maxDrift, double((measurement.angularMomentum - momentum).length()) / denom);
                sourcePrevious.evaluate(float(f - .5) / ratio);
                sourceNext.evaluate(float(f + .5) / ratio);
                sourceCenter.evaluate(float(f) / ratio);
                need(b.pose(sourcePrevious, q0.data(), error) && b.pose(sourceNext, q1.data(), error), error);
                mj_differentiatePos(m, d->qvel, 1. / out.fps, q0.data(), q1.data());
                need(b.pose(sourceCenter, d->qpos, error), error);
                sourceDrift = std::max(
                    sourceDrift, double((b.measure(d.get()).angularMomentum - momentum).length()) / denom);
            }
        float interpolatedError = report.maxBallisticError;
        Scene interpolated = out.motionOnlyCopy();
        for (double frame = first * ratio + .25; frame < last * ratio; frame += .25) {
            interpolated.evaluate(float(frame));
            double t = (frame - first * ratio) / out.fps;
            auto expected = start + velocity * float(t) + gravity * float(.5 * t * t);
            interpolatedError = std::max(interpolatedError,
                                         (physicalCenterOfMass(interpolated) * b.scale - expected).length());
        }
        report.angularMomentumDrift = float(maxDrift);
        report.status = std::max(report.maxBallisticError, interpolatedError) <= 1e-4 * H &&
                                (!settings.conserveAngularMomentum || maxDrift <= .01) &&
                                report.maxLimitError <= .5
                            ? "solved"
                            : "approximate";
        report.stopReason = "Analytical COM trajectory; internal shape velocity compensated using the "
                            "angular momentum matrix.";
        report.elapsedMs = timer.elapsed();
        report.details = {{"interpolatedBallisticErrorM", interpolatedError},
                          {"mode", settings.initialVelocityMode ? "initial-velocity" : "endpoint-positions"},
                          {"initialVelocityMS", vec(velocity)},
                          {"endVelocityMS", vec(calculatedEndVelocity)},
                          {"endVelocityResidualMS", settings.constrainEndVelocity ? velocityError : 0},
                          {"initialAngularMomentumKgM2S", vec(momentum)},
                          {"angularMomentumRelativeDrift", maxDrift},
                          {"conserveAngularMomentum", settings.conserveAngularMomentum},
                          {"externalAnchors", false},
                          {"rootActuators", 0},
                          {"trajectory", trajectories},
                          {"angularSubstepSeconds", dt},
                          {"outputFps", out.fps}};
        report.details["beforeBallisticErrorM"] = sourceBallisticError;
        report.details["beforeAngularMomentumRelativeDrift"] =
            settings.conserveAngularMomentum ? QJsonValue(sourceDrift) : QJsonValue(QJsonValue::Null);
        if (!settings.conserveAngularMomentum)
            report.details["angularMomentumRelativeDrift"] = QJsonValue(QJsonValue::Null);
        recordPhysicsProvenance(source, first, last, report, b.xml);
        report.details["dynamicValidation"] =
            "flight invariants only; internal torque feasibility is not certified";
        out.physicsResult = report.json();
        out.name = source.name + " - ballistic flight correction";
        out.evaluate(first * ratio);
        need(out.validate(&error), error);
        result = std::move(out);
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
} // namespace trickster
