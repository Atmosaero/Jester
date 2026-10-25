#include "core/scene/runtime_error.h"
#include "physics/physics.h"
#include "core/scene/core.h"
#include "physics/mujoco_backend.h"
#include "physics/physics_provenance.h"
#include "physics/physics_tools.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace trickster {
namespace {
void require(bool ok, const QString &message) {
    if (!ok)
        throw trickster::RuntimeError(message.toStdString());
}
std::vector<RigidSegment> rig(const Scene &s) {
    return mj::definitions(s);
}
QJsonArray vec(QVector3D v) {
    return {v.x(), v.y(), v.z()};
}
} // namespace
std::vector<RigidSegment> fittedPhysicsRig(const Scene &s, float totalMass) {
    std::vector<RigidSegment> out;
    if (s.nodes.empty())
        return out;
    const auto profile = s.profile.configured ? s.profile : s.suggestedProfile();
    const bool useProfile = s.profile.configured || profile.roles.size() >= 5;
    auto g = s.globals(s.restPose());
    auto p = s.positions(s.restPose());
    float h = s.height(), sum = 0;
    for (int i = 0; i < int(s.nodes.size()); ++i) {
        if (!s.nodes[i].joint)
            continue;
        if (useProfile && profile.roles.value("root", -1) == i && profile.roles.value("pelvis", -1) != i)
            continue;
        if (useProfile && !profile.roles.values().contains(i))
            continue;
        QVector3D end;
        float longest = 0;
        for (int j = 0; j < int(s.nodes.size()); ++j)
            if (s.nodes[j].joint && j != i && [&] {
                    if (!useProfile)
                        return s.nodes[j].parent == i;
                    if (!profile.roles.values().contains(j))
                        return false;
                    int ancestor = s.nodes[j].parent;
                    while (ancestor >= 0 && !profile.roles.values().contains(ancestor))
                        ancestor = s.nodes[ancestor].parent;
                    return ancestor == i;
                }()) {
                float length = (p[j] - p[i]).length();
                if (length > longest) {
                    longest = length;
                    end = g[i].inverted().map(p[j]);
                }
            }
        // A documented capsule fit is only an initial rig. User-entered masses and sizes are authoritative.
        float radius = std::clamp(longest * .22f, h * .008f, h * .055f);
        auto worldRadius = radius;
        float volume = 3.14159265f * worldRadius * worldRadius * (longest + 4.f * worldRadius / 3.f);
        float scale = std::max(g[i].mapVector({1, 0, 0}).length(), .00001f);
        out.push_back({i, volume, radius / scale, end * .5f, end});
        sum += volume;
    }
    for (auto &b : out)
        b.mass = totalMass * b.mass / std::max(sum, 1e-12f);
    return out;
}
std::vector<RigidSegment> humanPhysicsRig(const Scene &s, float totalMass) {
    auto profile = s.profile.configured ? s.profile : s.suggestedProfile();
    require(profile.roles.contains("pelvis") && profile.roles.contains("head") &&
                profile.roles.contains("leftFoot") && profile.roles.contains("rightFoot"),
            "Review pelvis, head and both foot roles before choosing the human mass estimate.");
    auto out = fittedPhysicsRig(s, totalMass);
    QMap<QString, float> fractions{{"pelvis", 14}, {"spine", 10}, {"chest", 20}, {"neck", 2}, {"head", 8}};
    for (QString side : {"left", "right"}) {
        fractions[side + "Shoulder"] = 1;
        fractions[side + "UpperArm"] = 2.7f;
        fractions[side + "ForeArm"] = 1.6f;
        fractions[side + "Hand"] = .6f;
        fractions[side + "UpperLeg"] = 10;
        fractions[side + "Leg"] = 4.7f;
        fractions[side + "Foot"] = 1.4f;
    }
    float sum = 0;
    for (auto &body : out) {
        float weight = .1f;
        for (auto it = profile.roles.begin(); it != profile.roles.end(); ++it)
            if (it.value() == body.node)
                weight = std::max(weight, fractions.value(it.key(), .1f));
        body.mass = weight;
        body.estimated = true;
        sum += weight;
    }
    for (auto &body : out)
        body.mass = body.mass * totalMass / sum;
    return out;
}
std::vector<Contact> physicsContactSchedule(const Scene &s) {
    if (!s.contacts.empty())
        return s.contacts;
    std::vector<Contact> out;
    double ratio = double(s.fps) / s.physicsResult["sourceFps"].toDouble(s.fps);
    for (auto value : s.physicsResult["contactSchedule"].toArray()) {
        auto o = value.toObject();
        Contact c;
        c.node = o["node"].toInt(-1);
        if (c.node < 0 || c.node >= int(s.nodes.size()))
            continue;
        c.first = qRound(o["first"].toDouble() * ratio);
        c.last = qRound(o["last"].toDouble() * ratio);
        c.fadeIn = qRound(o["fadeIn"].toDouble() * ratio);
        c.fadeOut = qRound(o["fadeOut"].toDouble() * ratio);
        auto t = o["target"].toArray(), p = o["pole"].toArray(), r = o["orientation"].toArray();
        if (t.size() != 3)
            continue;
        c.target = {float(t[0].toDouble()), float(t[1].toDouble()), float(t[2].toDouble())};
        if (p.size() == 3)
            c.pole = {float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble())};
        c.orient = o["orient"].toBool();
        if (r.size() == 4)
            c.orientation = {float(r[3].toDouble()), float(r[0].toDouble()), float(r[1].toDouble()),
                             float(r[2].toDouble())};
        out.push_back(c);
    }
    return out;
}
void fitFootPads(Scene &s) {
    if (s.bodies.empty())
        s.bodies = fittedPhysicsRig(s);
    auto rest = s;
    s.pose = s.restPose();
    auto g = s.globals(s.pose);
    auto p = s.positions(s.pose);
    float h = s.height();
    int count = 0;
    for (auto &body : s.bodies)
        if (s.isEffector(body.node) && s.isFoot(body.node)) {
            int node = body.node;
            auto rotation = s.globalRotation(node, s.pose);
            float scale = g[node].mapVector({1, 0, 0}).length();
            QVector3D forward(0, 0, 1);
            float longest = 0;
            for (int child = 0; child < int(s.nodes.size()); ++child)
                if (s.nodes[child].parent == node) {
                    auto delta = p[child] - p[node];
                    delta.setY(0);
                    if (delta.length() > longest) {
                        longest = delta.length();
                        forward = delta.normalized();
                    }
                }
            auto frame = QQuaternion::fromDirection(forward, {0, 1, 0});
            body.shapeRotation = (rotation.conjugated() * frame).normalized();
            float ankle = std::max(0.f, p[node].y() - s.physics.ground),
                  halfY = std::clamp(ankle * .5f, h * .012f, h * .03f);
            body.footHalfSize = {h * .035f / scale, halfY / scale, h * .07f / scale};
            auto center = forward * (h * .03f) + QVector3D(0, s.physics.ground + halfY - p[node].y(), 0);
            body.center = g[node].inverted().mapVector(center);
            body.inertia = {};
            body.estimated = true;
            ++count;
        }
    s.pose = rest.pose;
    require(count > 0, "Assign both foot roles before fitting support pads.");
}
QVector3D groundContactTarget(const Scene &s, int node, QVector3D target) {
    target.setY(s.physics.ground);
    for (const auto &body : s.bodies)
        if (body.node == node && body.footHalfSize.lengthSquared() > 0) {
            auto g = s.globals(s.pose)[node];
            auto R = body.shapeRotation.normalized();
            float lower = g.mapVector(body.center).y();
            for (int k = 0; k < 3; ++k) {
                QVector3D axis;
                axis[k] = body.footHalfSize[k];
                lower -= std::abs(g.mapVector(R.rotatedVector(axis)).y());
            }
            target.setY(s.physics.ground - lower);
            break;
        }
    return target;
}
float physicalMass(const Scene &s) {
    float sum = 0;
    for (const auto &b : rig(s))
        sum += b.mass;
    return sum;
}
QVector3D physicalCenterOfMass(const Scene &s) {
    auto g = s.globals(s.pose);
    QVector3D c;
    float sum = 0;
    for (const auto &b : rig(s)) {
        c += g[b.node].map(b.center) * b.mass;
        sum += b.mass;
    }
    return sum > 0 ? c / sum : QVector3D{};
}

void preservePhysicsTimebase(const Scene &source,Scene &out) {
    if(!source.physics.hasJointModel) {
        out.physics.hasJointModel=true;out.physics.modelJointLimits=source.jointLimits;
        out.physics.modelRotationLimits=source.rotationLimits;
    }
    const double nativeRate=double(source.fps)*source.physics.substeps;
    const int substeps=int(std::ceil(nativeRate/out.fps-1e-10));
    require(substeps>=1&&substeps<=512,"Requested FPS cannot retain the numerical timestep within 512 substeps/frame.");
    out.physics.substeps=substeps;
    if(std::abs(double(out.fps)*substeps-nativeRate)>1e-8)
        out.warnings<<QString("Resampling changes the numerical rate from %1 to %2 Hz; physics validation is stale.").arg(nativeRate).arg(out.fps*substeps);
    if(!out.physicsResult.isEmpty()) {
        out.physicsResult["validationStale"]=true;
        out.physicsResult["staleReason"]="Animation representation or sample rate changed";
    }
}
Scene physicsBake(const Scene &source, PhysicsControl *control, int outputFps) {
    if (outputFps == 0)
        outputFps = source.fps;
    require(outputFps >= 1 && outputFps <= 240 &&
                qint64(source.last - source.first + 1) * outputFps / source.fps * source.nodes.size() <=
                    5000000,
            "Physics output is too large; shorten the clip or reduce output FPS.");
    Scene out = source, sample = source.motionOnlyCopy();
    sample.compareOriginal = false;
    out.keys.clear();
    out.curves.clear();
    out.curveBasePose.clear();
    out.curveBaseMorphs.clear();out.curveBasePins.clear();
    out.clearCorrections();
    out.clips.reset();
    out.sourceClip = -1;
    out.fps = outputFps;
    preservePhysicsTimebase(source,out);
    out.first = qRound(double(source.first) * outputFps / source.fps);
    out.last = qRound(double(source.last) * outputFps / source.fps);
    for (int f = out.first; f <= out.last; ++f) {
        require(!control || !control->cancel.load(), "Physics cancelled.");
        sample.evaluate(float(f) * source.fps / outputFps);
        out.keys[f] = {sample.pose, {}, sample.morphs};
    }
    out.contacts.clear();
    out.pins.clear();
    out.rotationLimits.clear();
    out.jointLimits.clear();
    out.bodyAssist = false;
    out.evaluate(out.first);
    return out;
}
QString physicsExportStatus(const Scene &scene) {
    if(scene.physicsResult.isEmpty())return scene.physics.requests.contains("rigProxy")
        ? " ? approximate rigid proxy; exported dynamics not checked" : QString{};
    auto r=scene.physicsResult;
    QString status=r["validationStale"].toBool()?"stale":r["status"].toString("not checked");
    auto exportDomain=r["validationDomains"].toObject()["exportDynamics"].toObject();
    QString result=exportDomain["status"].toString("not checked");
    QString knots=scene.physics.requests["trajectoryExport"].toObject()["mode"]=="engine-compatible"
        ? "; extra event keys retained" : QString{};
    return " ? physics: "+status+"; exported dynamics: "+result+knots;
}
QString physicsBackendVersion() {
    return QString("MuJoCo %1").arg(mj_versionString());
}
QJsonObject PhysicsMeasurement::json() const {
    QJsonArray tensor;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            tensor.append(inertia(i, j));
    return {{"massKg", mass},
            {"comM", vec(com)},
            {"linearMomentumKgMS", vec(linearMomentum)},
            {"angularMomentumKgM2S", vec(angularMomentum)},
            {"inertiaKgM2", tensor},
            {"penetrationM", penetration},
            {"limitErrorDeg", limitError},
            {"rootForceN", rootForce},
            {"rootTorqueNm", rootTorque},
            {"contactForces", contactForces},
            {"requiredJointTorques", requiredJointTorques},
            {"requiredRootWrenches", requiredRootWrenches},
            {"warnings", QJsonArray::fromStringList(warnings)}};
}
QJsonObject PhysicsReport::json() const {
    auto out = details;
    out["environment"] = physicsEnvironment();
    out["backend"] = backend;
    out["status"] = status;
    out["stopReason"] = stopReason;
    out["frames"] = frames;
    out["dofs"] = dofs;
    out["bodies"] = bodies;
    out["massKg"] = totalMass;
    out["elapsedMs"] = elapsedMs;
    out["contactErrorM"] = maxContactError;
    out["jointErrorM"] = maxJointError;
    out["penetrationM"] = maxPenetration;
    out["ballisticErrorM"] = ballistic ? QJsonValue(maxBallisticError) : QJsonValue(QJsonValue::Null);
    out["directedLimitErrorDeg"] = maxLimitError;
    out["externalAnchorForceN"] = maxAnchorForce;
    out["maxJointTorqueNm"] = maxTorque;
    out["targetErrorM"] = maxTargetPosition;
    out["targetErrorDeg"] = (!details["staticHintOnly"].toBool() || details["targetErrorDeg"].isDouble())
                                ? QJsonValue(maxTargetAngle)
                                : QJsonValue(QJsonValue::Null);
    bool dynamicMeasurement = !ballistic && !details["staticHintOnly"].toBool() && frames > 1;
    out["dynamicsMeasured"] = dynamicMeasurement;
    bool rootMeasured = details["afterRootForceRmsN"].isDouble();
    out["rootResidualMeasured"] = rootMeasured;
    out["rootForceRmsN"] = rootMeasured ? QJsonValue(rootForceRms) : QJsonValue(QJsonValue::Null);
    out["rootTorqueRmsNm"] = rootMeasured ? QJsonValue(rootTorqueRms) : QJsonValue(QJsonValue::Null);
    if (!dynamicMeasurement)
        out["maxJointTorqueNm"] = QJsonValue(QJsonValue::Null);
    out["warnings"] = QJsonArray::fromStringList(warnings);
    return out;
}
QString PhysicsReport::summary() const {
    if (details["staticHintOnly"].toBool())
        return QString("Whole-body · %1\nSupports %2 mm · target %3 mm\n%4\nStatic pose check; dynamics not "
                       "validated.")
            .arg(status)
            .arg(maxContactError * 1000, 0, 'f', 2)
            .arg(maxTargetPosition * 1000, 0, 'f', 2)
            .arg(stopReason);
    if (ballistic)
        return QString("Flight · %1\nCOM keys %2 mm · interpolated %3 mm\nAngular momentum drift "
                       "%4%\nInternal torque feasibility not certified.")
            .arg(status)
            .arg(maxBallisticError * 1000, 0, 'f', 3)
            .arg(details["interpolatedBallisticErrorM"].toDouble() * 1000, 0, 'f', 3)
            .arg(details["conserveAngularMomentum"].toBool()
                     ? QString::number(angularMomentumDrift * 100, 'f', 3)
                     : QString("not measured"));

    return QString("%1 · %2\n%3 frames · %4 bodies · %5 DOF · %6 kg\nContact %7 mm · penetration %8 "
                   "mm\nBallistic %9 mm · directed limit %10°\nExternal anchors %11 N · joint torque %12 "
                   "Nm\n%13\n%14")
        .arg(backend, status)
        .arg(frames)
        .arg(bodies)
        .arg(dofs)
        .arg(totalMass, 0, 'f', 2)
        .arg(maxContactError * 1000, 0, 'f', 2)
        .arg(maxPenetration * 1000, 0, 'f', 2)
        .arg(maxBallisticError * 1000, 0, 'f', 3)
        .arg(maxLimitError, 0, 'f', 2)
        .arg(maxAnchorForce, 0, 'f', 2)
        .arg(maxTorque, 0, 'f', 2)
        .arg(stopReason, warnings.join("\n"));
}
bool physicsModelXml(const Scene &s, QString &xml, QString &error, bool anchors) {
    mj::Backend b;
    if (!b.compile(s, error, anchors))
        return false;
    xml = b.xml;
    return true;
}
bool measurePhysics(const Scene &s, float frame, PhysicsMeasurement &out, QString &error) {
    if (frame < s.first || frame > s.last) {
        error = "Measurement frame is outside the clip.";
        return false;
    }
    mj::Backend b;
    if (!b.compile(s, error, false))
        return false;
    auto d = b.data();
    auto m = b.model.get();
    Scene a = s.motionOnlyCopy(), c = s.motionOnlyCopy();
    a.compareOriginal = false;
    c.compareOriginal = false;
    float lo = std::max(float(s.first), frame - 1.f), hi = std::min(float(s.last), frame + 1.f);
    a.evaluate(lo);
    c.evaluate(hi);
    std::vector<double> q0(m->nq), q1(m->nq), v0(m->nv), v1(m->nv), acc(m->nv);
    if (!b.pose(a, q0.data(), error) || !b.pose(c, q1.data(), error))
        return false;
    if (hi > lo)
        mj_differentiatePos(m, d->qvel, (hi - lo) / s.fps, q0.data(), q1.data());
    a.evaluate(frame);
    if (!b.pose(a, d->qpos, error))
        return false;
    if (frame > lo && hi > frame) {
        mj_differentiatePos(m, v0.data(), (frame - lo) / s.fps, q0.data(), d->qpos);
        mj_differentiatePos(m, v1.data(), (hi - frame) / s.fps, d->qpos, q1.data());
        for (int k = 0; k < m->nv; ++k)
            acc[k] = 2 * (v1[k] - v0[k]) * s.fps / (hi - lo);
    }
    out = b.measure(d.get());
    out.limitError = b.limitError(a);
    mju_copy(d->qacc, acc.data(), m->nv);
    mj_inverse(m, d.get());
    if (frame == lo || frame == hi)
        out.warnings << "Boundary acceleration is unavailable from centered differences and is assumed zero; "
                        "inverse residual is a boundary estimate.";
    for (int j = 0; j < m->njnt; ++j) {
        int v = m->jnt_dofadr[j];
        if (m->jnt_type[j] == mjJNT_FREE) {
            QVector3D force(float(d->qfrc_inverse[v]), float(d->qfrc_inverse[v + 1]),
                            float(d->qfrc_inverse[v + 2])),
                torque(float(d -> qfrc_inverse[v + 3]), float(d->qfrc_inverse[v + 4]),
                       float(d->qfrc_inverse[v + 5]));
            out.rootForce = std::max(out.rootForce, force.length());
            out.rootTorque = std::max(out.rootTorque, torque.length());
            out.requiredRootWrenches.append(
                QJsonObject{{"joint", j}, {"forceN", vec(force)}, {"torqueNm", vec(torque)}});
        }
    }
    for (int u = 0; u < m->nu; ++u) {
        int j = m->actuator_trnid[2 * u], v = m->jnt_dofadr[j];
        double torque = 0;
        if (m->jnt_type[j] == mjJNT_BALL)
            for (int k = 0; k < 3; ++k)
                torque += m->actuator_gear[6 * u + k] * d->qfrc_inverse[v + k];
        else
            torque = d->qfrc_inverse[v];
        out.requiredJointTorques.append(QJsonObject{
            {"actuator", u}, {"requiredNm", torque}, {"limitNm", m->actuator_forcerange[2 * u + 1]}});
    }
    for (int i = 0; i < d->ncon; ++i) {
        double force[6];
        mj_contactForce(m, d.get(), i, force);
        const auto &c = d->contact[i];
        out.contactForces.append(QJsonObject{
            {"positionM", QJsonArray{c.pos[0], c.pos[1], c.pos[2]}},
            {"normalN", force[0]},
            {"tangentN", std::hypot(force[1], force[2])},
            {"source", "inverse dynamics of sampled authored motion; not a forward certificate"}});
    }
    return true;
}
bool correctBallistic(const Scene &source, int first, int last, Scene &result, PhysicsReport &report,
                      QString &error, PhysicsControl *control) {
    FlightSettings settings;
    settings.conserveAngularMomentum = false;
    settings.outputFps = source.fps;
    return solveFlight(source, first, last, settings, result, report, error, control);
}
bool simulatePhysics(const Scene &source, int first, int last, Scene &result, PhysicsReport &report,
                     QString &error, PhysicsControl *control) {
    try {
        // Legacy equality anchors need a fine numerical step; do not alter any mechanical parameter.
        if (source.physics.externalAnchors && source.physics.substeps < 64 &&
            (!source.contacts.empty() || !source.pins.empty())) {
            Scene refined = source;
            refined.physics.substeps = 64;
            if (!simulatePhysics(refined, first, last, result, report, error, control))
                return false;
            report.details["requestedSubsteps"] = source.physics.substeps;
            report.details["actualSubsteps"] = 64;
            report.warnings << "Legacy anchor preview uses 64 numerical substeps for stability; requested "
                               "authoring setting is preserved. Mechanical parameters unchanged.";
            recordPhysicsProvenance(source, first, last, report, QString());
            result.physics.substeps = source.physics.substeps;
            result.physicsResult = report.json();
            return true;
        }
        QElapsedTimer timer;
        timer.start();
        require(source.validate(&error), error);
        require(first >= source.first && last <= source.last && last > first,
                "Choose at least two simulation frames.");
        require(qint64(source.last - source.first + 1) * source.nodes.size() <= 5000000,
                "Range too large to bake.");
        mj::Backend b;
        require(b.compile(source, error, source.physics.externalAnchors), error);
        auto data = b.data();
        auto m = b.model.get();
        auto d = data.get();
        Scene target = source, next = source;
        target.compareOriginal = false;
        next.compareOriginal = false;
        target.evaluate(first);
        next.evaluate(std::min(last, first + 1));
        std::vector<double> qnext(m->nq), desired(m->nq), dv(m->nv), err(m->nv);
        require(b.pose(target, d->qpos, error), error);
        require(b.pose(next, qnext.data(), error), error);
        mj_differentiatePos(m, d->qvel, 1. / source.fps, d->qpos, qnext.data());
        b.updateAnchors(target, float(first), d);
        mj_forward(m, d);
        Scene out = physicsBake(source, control);
        out.bodies = mj::definitions(source);
        report = {};
        report.backend = physicsBackendVersion();
        report.frames = last - first + 1;
        report.bodies = int(b.bodies.size());
        report.dofs = m->nv;
        report.totalMass = physicalMass(source);
        report.following = source.physics.followAnimation;
        report.warnings = source.physics.migrationWarnings;
        if (!b.anchors.empty())
            report.warnings << "EXTERNAL ANCHOR preview: authored pins/contacts are world constraints, not "
                               "ground reaction validation.";
        if (!source.jointLimits.isEmpty())
            report.warnings << "Three-hinge joints bound asymmetric twist and two swing axes; deviation from "
                               "editor cone is measured separately.";
        if (source.physics.restitution > 0)
            report.warnings << "Legacy restitution is retained as authored data; MuJoCo uses contact "
                               "time/damping instead.";
        std::vector<double> limits = b.driveLimits(source);
        QJsonArray anchorWrenches;
        double anchorMoment = 0;
        double dt = m->opt.timestep;
        for (int f = first; f <= last; ++f) {
            require(!control || !control->cancel.load(), "Physics cancelled.");
            if (control)
                control->progress.store(100 * (f - first) / std::max(1, last - first));
            if (f > first)
                for (int sub = 0; sub < source.physics.substeps; ++sub) {
                    require(!control || !control->cancel.load(), "Physics cancelled.");
                    float t = float(f - 1) + float(sub) / source.physics.substeps;
                    target.evaluate(t);
                    next.evaluate(std::min(float(last), t + 1.f / source.physics.substeps));
                    require(b.pose(target, desired.data(), error), error);
                    require(b.pose(next, qnext.data(), error), error);
                    b.updateAnchors(target, t, d);
                    for (int u = 0; u < m->nu; ++u)
                        d->ctrl[u] =
                            0; // Passive mode retains only the authored, bounded internal viscous damping.
                    if (source.physics.followAnimation) {
                        mj_differentiatePos(m, err.data(), 1, d->qpos, desired.data());
                        mj_differentiatePos(m, dv.data(), dt, desired.data(), qnext.data());
                        for (int u = 0; u < m->nu; ++u) {
                            int joint = m->actuator_trnid[2 * u], vadr = m->jnt_dofadr[joint];
                            if (m->jnt_type[joint] == mjJNT_BALL)
                                for (int k = 0; k < 3; ++k)
                                    if (m->actuator_gear[6 * u + k] > .5)
                                        vadr += k;
                            int body = m->jnt_bodyid[joint];
                            const auto &def =
                                b.bodies[b.indices[mj_id2name(m, mjOBJ_BODY, body)
                                                       ? QString::fromUtf8(mj_id2name(m, mjOBJ_BODY, body))
                                                             .mid(1)
                                                             .toInt()
                                                       : -1]]
                                    .definition;
                            double kp = def.stiffness < 0 ? source.physics.stiffness : def.stiffness;
                            double kd = def.damping < 0 ? source.physics.damping : def.damping;
                            double torque = kp * err[vadr] + kd * dv[vadr];
                            // MuJoCo differentiates the velocity bias implicitly; the complete actuator force
                            // is bounded.
                            d->ctrl[u] = torque;
                            report.maxTorque = std::max(report.maxTorque,
                                                        float(std::abs(std::clamp(torque - kd * d->qvel[vadr],
                                                                                  -limits[u], limits[u]))));
                        }
                    }
                    mj_step(m, d);
                    for (int u = 0; u < m->nu; ++u)
                        report.maxTorque = std::max(report.maxTorque, float(std::abs(d->actuator_force[u])));
                    require(d->warning[mjWARN_BADQPOS].number == 0 &&
                                d->warning[mjWARN_BADQVEL].number == 0 &&
                                d->warning[mjWARN_BADQACC].number == 0,
                            "MuJoCo reported invalid dynamics; no automatic reset/result accepted.");
                    for (int k = 0; k < m->nv; ++k)
                        require(std::isfinite(d->qvel[k]) && std::abs(d->qvel[k]) < 1e6,
                                "Dynamics diverged; reduce stiffness or increase substeps.");
                }
            mj_forward(m, d);
            out.evaluate(f);
            b.reconstruct(out, d);
            out.keys[f] = {out.pose, {}, out.morphs};
            report.maxLimitError = std::max(report.maxLimitError, b.limitError(out));
            auto measurement = b.measure(d);
            report.maxPenetration = std::max(report.maxPenetration, measurement.penetration);
            auto p = out.positions(out.pose);
            target.evaluate(f);
            auto wanted = target.positions(target.pose);
            for (const auto &body : b.bodies) {
                int node = body.definition.node;
                report.maxJointError = std::max(
                    report.maxJointError, (p[node] * b.scale - QVector3D(float(d->xpos[3 * body.id]),
                                                                         float(d->xpos[3 * body.id + 1]),
                                                                         float(d->xpos[3 * body.id + 2])))
                                              .length());
                report.maxTargetPosition =
                    std::max(report.maxTargetPosition, (p[node] - wanted[node]).length() * b.scale);
                auto delta = mj::worldRotation(target, node) * mj::worldRotation(out, node).conjugated();
                report.maxTargetAngle =
                    std::max(report.maxTargetAngle,
                             float(qRadiansToDegrees(
                                 2 * std::acos(std::clamp(double(std::abs(delta.scalar())), 0., 1.)))));
            }
            for (const auto &a : b.anchors)
                if (d->eq_active[a.equality]) {
                    auto targetPoint =
                        QVector3D(float(d->mocap_pos[3 * a.mocap]), float(d->mocap_pos[3 * a.mocap + 1]),
                                  float(d->mocap_pos[3 * a.mocap + 2]));
                    report.maxContactError =
                        std::max(report.maxContactError, (p[a.node] * b.scale - targetPoint).length());
                    if (a.contact.orient) {
                        auto delta = a.contact.orientation * mj::worldRotation(out, a.node).conjugated();
                        report.maxContactAngle = std::max(
                            report.maxContactAngle,
                            float(qRadiansToDegrees(
                                2 * std::acos(std::clamp(double(std::abs(delta.scalar())), 0., 1.)))));
                    }
                }
            // Constraint multipliers have residual-dependent units (weld orientation uses quaternion
            // residuals). Project each anchor through J^T before reporting an SI free-root wrench.
            if (!b.anchors.empty() && d->nefc > 0) {
                std::vector<double> rows(d->nefc), generalized(m->nv);
                for (const auto &anchor : b.anchors)
                    if (d->eq_active[anchor.equality]) {
                        for (int row = 0; row < d->nefc; ++row)
                            rows[row] =
                                d->efc_type[row] == mjCNSTR_EQUALITY && d->efc_id[row] == anchor.equality
                                    ? d->efc_force[row]
                                    : 0;
                        mj_mulJacTVec(m, d, generalized.data(), rows.data());
                        for (int j = 0; j < m->njnt; ++j)
                            if (m->jnt_type[j] == mjJNT_FREE) {
                                int v = m->jnt_dofadr[j];
                                QVector3D force{float(generalized[v]), float(generalized[v + 1]),
                                                float(generalized[v + 2])};
                                QVector3D moment{float(generalized[v + 3]), float(generalized[v + 4]),
                                                 float(generalized[v + 5])};
                                report.maxAnchorForce = std::max(report.maxAnchorForce, force.length());
                                anchorMoment = std::max(anchorMoment, double(moment.length()));
                                if (force.lengthSquared() + moment.lengthSquared() > 1e-16)
                                    anchorWrenches.append(
                                        QJsonObject{{"frame", f},
                                                    {"node", anchor.node},
                                                    {"rootJoint", j},
                                                    {"forceWorldN", vec(force)},
                                                    {"momentAboutRootNm", vec(moment)},
                                                    {"momentFrame", "root-local; about root origin"}});
                            }
                    }
            }
        }
        out.name = source.name + (source.physics.followAnimation ? " - dynamics preview (MuJoCo follow)"
                                                                 : " - dynamics preview (MuJoCo passive)");
        out.evaluate(first);
        require(out.validate(&error), error);
        report.elapsedMs = timer.elapsed();
        report.stopReason = "Forward simulation completed; not trajectory optimization.";
        report.details = {{"substepSeconds", dt},
                          {"solverIterations", m->opt.iterations},
                          {"externalAnchors", !b.anchors.empty()},
                          {"externalAnchorMomentNm", anchorMoment},
                          {"externalAnchorWrenches", anchorWrenches}};
        report.details["dynamicValidation"] =
            "Forward preview only; not a pose/contact feasibility certificate.";
        recordPhysicsProvenance(source, first, last, report, b.xml);
        out.physicsResult = report.json();
        result = std::move(out);
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        // Numerical refinement is confined to this preview; optimization never changes its fixed timestep.
        // Every retry starts from a fresh model/data and the original pose, never a reset diverged state.
        if ((!control || !control->cancel.load()) && source.physics.substeps < 64 &&
            (error.contains("diverged") || error.contains("invalid dynamics"))) {
            Scene refined = source;
            refined.physics.substeps = std::min(64, source.physics.substeps * 2);
            QString retryError;
            if (simulatePhysics(refined, first, last, result, report, retryError, control)) {
                report.details["requestedSubsteps"] = source.physics.substeps;
                report.details["actualSubsteps"] =
                    report.details["actualSubsteps"].toInt(refined.physics.substeps);
                report.warnings << QString("Preview restarted after numerical instability: requested %1 "
                                           "substeps, used %2. Mechanical parameters unchanged.")
                                       .arg(source.physics.substeps)
                                       .arg(report.details["actualSubsteps"].toInt());
                // Rebuild the successful model XML while restoring the original input identity.
                QString modelXml, ignored;
                Scene compiled = source;
                compiled.physics.substeps = report.details["actualSubsteps"].toInt();
                physicsModelXml(compiled, modelXml, ignored, source.physics.externalAnchors);
                recordPhysicsProvenance(source, first, last, report, modelXml);
                result.physics.substeps = source.physics.substeps;
                result.physicsResult = report.json();
                return true;
            }
            error = retryError;
        }
        return false;
    }
}
} // namespace trickster
