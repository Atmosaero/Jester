#include "runtime_error.h"
#include "mujoco_backend.h"
#include "physics_provenance.h"
#include "physics_replay.h"
#include "physics_tools.h"
#include "physics_trajectory.h"
#include "physics_validation.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSet>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
namespace trickster {
namespace {
void need(bool ok, const QString &e) {
    if (!ok)
        throw trickster::RuntimeError(e.toStdString());
}
struct Target {
    std::vector<double> q, v, a, feed;
    double weight = 1;
    QVector3D com, comVelocity;
};
struct Rollout {
    double cost = 0, rootForce2 = 0, rootTorque2 = 0, maxRootForce = 0, maxRootTorque = 0, maxTorque = 0,
           penetration = 0, contactError = 0, goalError = 0, limitError = 0;
    double frictionError = 0, wrenchRatio = 0, flightForce = 0, normalMin = 0, goalAngle = 0,
           transitionError = 0, endComSpeed = 0, endAngularSpeed = 0;
    std::vector<std::vector<double>> q, controls;
    QJsonArray forces, importantErrors;
    double contactPeakFrame = 0;
    int contactPeakNode = -1;
};

} // namespace
bool optimizeDynamics(const Scene &source, int first, int last, const DynamicsSettings &settings,
                      Scene &result, PhysicsReport &report, QString &error, PhysicsControl *control) {
    try {
        QElapsedTimer timer;
        timer.start();
        need(source.validate(&error), error);
        need(first >= source.first && last <= source.last && first < last,
             "Choose a correction interval of two or more frames.");
        need(settings.iterations >= 1 && settings.iterations <= 200 && std::isfinite(settings.seconds) &&
                 settings.seconds > 0 && settings.seconds <= 300,
             "Invalid optimization budget.");
        need(settings.outputFps >= 0 && settings.outputFps <= 240 && std::isfinite(settings.poseWeight) &&
                 settings.poseWeight > 0 && std::isfinite(settings.contactWeight) &&
                 settings.contactWeight > 0 && std::isfinite(settings.smoothWeight) &&
                 settings.smoothWeight >= 0,
             "Invalid dynamics weights/output FPS.");
        need(last - first <= 300, "Trajectory optimization is limited to 300 frames per interval.");
        Scene driven = source;
        driven.physics.followAnimation = true;
        mj::Backend b;
        need(b.compile(driven, error, false), error);
        auto m = b.model.get();
        int nv = m->nv, nq = m->nq, nu = m->nu;
        need(nu > 0,
             "Dynamics optimization requires internal joint actuators; a free root is never actuated.");
        for (auto it = settings.importantFrames.begin(); it != settings.importantFrames.end(); ++it)
            need(it.key() >= first && it.key() <= last && std::isfinite(it.value()) && it.value() >= 0,
                 "Important pose frames must be inside the interval with nonnegative weights.");
        const auto schedule = physicsContactSchedule(source);
        int sub = source.physics.substeps, steps = (last - first) * sub;
        double dt = m->opt.timestep;
        need(qint64(steps + 1) * (nq + 3 * nv + nu) <= 20000000,
             "Optimization snapshot exceeds the bounded memory budget.");
        double H = std::max(.01f, source.height() * b.scale), M = physicalMass(source),
               G = std::max(1.f, source.physics.gravity);
        std::vector<int> rootDofs;
        for (int j = 0; j < m->njnt; ++j)
            if (m->jnt_type[j] == mjJNT_FREE)
                rootDofs.push_back(m->jnt_dofadr[j]);
        auto data = b.data(), inverse = b.data();
        auto d = data.get();
        auto inv = inverse.get();
        // Optimize a smooth tracking reference before control shooting. Important poses and endpoints remain
        // fixed. The physical model, gravity, friction and contact schedule are unchanged.
        Scene reference = physicsBake(source, control);
        reference.contacts = schedule;
        reference.jointLimits = source.jointLimits;
        reference.rotationLimits = source.rotationLimits;
        int count = last - first + 1;
        int endHold = settings.endAtRest
                          ? std::min(std::max(2, int(std::ceil(source.fps * .2))), std::max(2, count / 3))
                          : 2;
        if (settings.endAtRest)
            for (int f = std::max(first + 2, last - endHold + 1); f < last; ++f)
                if (!settings.importantFrames.contains(f))
                    reference.keys[f].pose = reference.keys[last].pose;
        QJsonArray preparation;
        QSet<int> preparedFrames;
        // Prepare articulated legs before an authored impact. Merely tracking the original straight-leg
        // touchdown drives a knee into its hard stop. This changes reference poses, never the model,
        // contact schedule, important poses or initial q/v (the first two source samples stay fixed).
        bool articulatedTorso = false;
        for (const auto &body : b.bodies)
            for (const auto &role : {"spine", "chest"})
                if (source.profile.roles.value(role, -1) == body.definition.node)
                    articulatedTorso = true;
        // A full torso carries pitch momentum independently of the pelvis. Folding its legs too
        // early delays support; retain a smaller flight adjustment until the scheduled plant.
        if (settings.prepareLanding && rootDofs.size() == 1)
            for (const auto &contact : schedule) {
                int plant = contact.first + contact.fadeIn;
                if (plant <= first + 2 || plant >= last)
                    continue;
                auto chain = source.ikChain(contact.node);
                if (chain.root < 0 || chain.mid < 0 || chain.end < 0)
                    continue;
                int begin = std::max(first + 1, plant - std::max(2, int(std::ceil(source.fps * .15))));
                int end = std::min(last - 1, plant + std::max(2, int(std::ceil(source.fps * .2))));
                int future = std::min(last, plant + std::max(2, int(std::ceil(source.fps * .25))));
                auto templatePose = reference.keys[future].pose;
                for (int f = begin + 1; f < end; ++f) {
                    if (settings.importantFrames.contains(f))
                        continue;
                    double phase =
                        f <= plant ? double(f - begin) / (plant - begin) : double(end - f) / (end - plant);
                    double maximumBlend = articulatedTorso && f < plant ? .15 : .6;
                    double blend = maximumBlend * phase * phase * (3 - 2 * phase);
                    auto &pose = reference.keys[f].pose;
                    for (int node : {chain.root, chain.mid, chain.end})
                        pose[node].r = QQuaternion::slerp(pose[node].r, templatePose[node].r, float(blend));
                    if (f >= plant) {
                        Scene aligned = reference;
                        aligned.pose = pose;
                        auto positions = aligned.positions(pose);
                        QVector3D shift(0, contact.target.y() - positions[contact.node].y(), 0);
                        for (const auto &body : b.bodies)
                            if (body.parent < 0) {
                                int node = body.definition.node, parent = aligned.nodes[node].parent;
                                pose[node].t +=
                                    parent < 0 ? shift
                                               : aligned.globals(pose)[parent].inverted().mapVector(shift);
                            }
                        preparedFrames.insert(f);
                    }
                }
                preparation.append(QJsonObject{{"node", contact.node},
                                               {"plantFrame", plant},
                                               {"templateFrame", future},
                                               {"begin", begin},
                                               {"end", end},
                                               {"maximumBlend", .6},
                                               {"airborneMaximumBlend", articulatedTorso ? .15 : .6},
                                               {"methodVersion", "landing-reference/2"}});
            }
        double smoothing = 400 * settings.smoothWeight * std::pow(source.fps / 30., 4);
        std::vector<double> smoothMatrix(count * count), smoothRhs(count), smoothSolution(count);
        for (const auto &body : b.bodies)
            if (body.parent < 0)
                for (int component = 0; component < 3; ++component) {
                    std::fill(smoothMatrix.begin(), smoothMatrix.end(), 0);
                    std::fill(smoothRhs.begin(), smoothRhs.end(), 0);
                    for (int f = 0; f < count; ++f) {
                        double weight =
                            (f <= 1 || f >= count - endHold || settings.importantFrames.contains(first + f) ||
                             preparedFrames.contains(first + f))
                                ? 1e8
                                : settings.poseWeight;
                        smoothMatrix[f * count + f] = weight;
                        smoothRhs[f] =
                            weight * reference.keys[first + f].pose[body.definition.node].t[component];
                    }
                    for (int f = 1; f < count - 1; ++f) {
                        double c[3] = {1, -2, 1};
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j)
                                smoothMatrix[(f - 1 + i) * count + f - 1 + j] += smoothing * c[i] * c[j];
                    }
                    mju_cholFactor(smoothMatrix.data(), count, 1e-12);
                    mju_cholSolve(smoothSolution.data(), smoothMatrix.data(), smoothRhs.data(), count);
                    for (int f = 0; f < count; ++f)
                        reference.keys[first + f].pose[body.definition.node].t[component] =
                            float(smoothSolution[f]);
                }
        std::vector<Target> targets;
        targets.resize(steps + 1);
        Scene s = reference, previous = reference, next = reference;
        s.compareOriginal = false;
        previous.compareOriginal = false;
        next.compareOriginal = false;
        std::vector<double> qminus(nq), qplus(nq), vminus(nv), vplus(nv), supportJac(3 * nv),
            supportRot(3 * nv), feedCorrection(nv);
        double beforeF = 0, beforeT = 0;
        // Cache each reference configuration once. Neighbor differences reused to evaluate the
        // reference three times per native sample, including repeated IK/contact solves.
        for (int i = 0; i <= steps; ++i) {
            need(!control || !control->cancel.load(), "Dynamics optimization cancelled.");
            s.evaluate(first + float(i) / sub);
            need(s.pins.isEmpty(), "Release kinematic world pins for dynamics correction.");
            targets[i].q.resize(nq);
            need(b.pose(s, targets[i].q.data(), error), error);
        }
        for (int i = 0; i <= steps; ++i) {
            need(!control || !control->cancel.load(), "Dynamics optimization cancelled.");
            float frame = first + float(i) / sub, eps = 1.f;
            Target t;
            t.q = targets[i].q;
            t.v.resize(nv);
            t.a.resize(nv);
            t.feed.resize(nu);
            if (i - sub >= 0)
                mju_copy(qminus.data(), targets[i - sub].q.data(), nq);
            else {
                previous.evaluate(std::max(float(source.first), frame - eps));
                need(b.pose(previous, qminus.data(), error), error);
            }
            if (i + sub <= steps)
                mju_copy(qplus.data(), targets[i + sub].q.data(), nq);
            else {
                next.evaluate(std::min(float(source.last), frame + eps));
                need(b.pose(next, qplus.data(), error), error);
            }
            double left = (frame - std::max(float(source.first), frame - eps)) / source.fps,
                   right = (std::min(float(source.last), frame + eps) - frame) / source.fps;
            if (left > 0)
                mj_differentiatePos(m, vminus.data(), left, qminus.data(), t.q.data());
            if (right > 0)
                mj_differentiatePos(m, vplus.data(), right, t.q.data(), qplus.data());
            for (int k = 0; k < nv; ++k) {
                if (left <= 0)
                    vminus[k] = vplus[k];
                if (right <= 0)
                    vplus[k] = vminus[k];
                t.v[k] = (vminus[k] + vplus[k]) * .5;
                t.a[k] = left > 0 && right > 0 ? (vplus[k] - vminus[k]) / ((left + right) * .5) : 0;
            }
            mju_copy(inv->qpos, t.q.data(), nq);
            mju_copy(inv->qvel, t.v.data(), nv);
            mju_copy(inv->qacc, t.a.data(), nv);
            mj_inverse(m, inv);
            mj_jacSubtreeCom(m, inv, supportJac.data(), 0);
            t.com = {float(inv->subtree_com[0]), float(inv->subtree_com[1]), float(inv->subtree_com[2])};
            for (int k = 0; k < 3; ++k)
                for (int v = 0; v < nv; ++v)
                    t.comVelocity[k] += float(supportJac[k * nv + v] * t.v[v]);
            for (int r : rootDofs) {
                double f2 = 0, t2 = 0;
                for (int k = 0; k < 3; ++k) {
                    f2 += inv->qfrc_inverse[r + k] * inv->qfrc_inverse[r + k];
                    t2 += inv->qfrc_inverse[r + 3 + k] * inv->qfrc_inverse[r + 3 + k];
                }
                beforeF += f2;
                beforeT += t2;
            }
            // Desired support wrench is only inverse-feedforward bookkeeping. It is never applied to a
            // body/root. Actual reactions still come exclusively from unilateral geom contacts in every
            // forward rollout.
            std::fill(feedCorrection.begin(), feedCorrection.end(), 0);
            std::vector<const Contact *> supports;
            for (const auto &c : schedule)
                if (contactWeight(c, frame) > 0 && b.indices.contains(c.node))
                    supports.push_back(&c);
            if (!supports.empty() && rootDofs.size() == 1) {
                int r = rootDofs.front();
                double force[3] = {inv->qfrc_inverse[r], std::max(0., inv->qfrc_inverse[r + 1]),
                                   inv->qfrc_inverse[r + 2]};
                double horizontal = std::hypot(force[0], force[2]),
                       bound = source.physics.friction * force[1];
                if (horizontal > bound && horizontal > 0) {
                    force[0] *= bound / horizontal;
                    force[2] *= bound / horizontal;
                }
                int rootBody = -1;
                for (const auto &body : b.bodies)
                    if (body.parent < 0) {
                        rootBody = body.id;
                        break;
                    }
                auto q =
                    QQuaternion(float(inv->xquat[4 * rootBody]), float(inv->xquat[4 * rootBody + 1]),
                                float(inv->xquat[4 * rootBody + 2]), float(inv->xquat[4 * rootBody + 3]));
                auto torque =
                    q.rotatedVector({float(inv->qfrc_inverse[r + 3]), float(inv->qfrc_inverse[r + 4]),
                                     float(inv->qfrc_inverse[r + 5])});
                for (auto c : supports) {
                    int site = b.bodies[b.indices[c->node]].site;
                    mj_jacSite(m, inv, supportJac.data(), supportRot.data(), site);
                    QVector3D arm(float(inv->site_xpos[3 * site] - inv->xpos[3 * rootBody]),
                                  float(inv->site_xpos[3 * site + 1] - inv->xpos[3 * rootBody + 1]),
                                  float(inv->site_xpos[3 * site + 2] - inv->xpos[3 * rootBody + 2]));
                    auto moment = (torque - QVector3D::crossProduct(
                                                arm, {float(force[0]), float(force[1]), float(force[2])})) /
                                  float(supports.size());
                    for (int v = 0; v < nv; ++v)
                        for (int k = 0; k < 3; ++k)
                            feedCorrection[v] += supportJac[k * nv + v] * force[k] / supports.size() +
                                                 supportRot[k * nv + v] * moment[k];
                }
            }
            for (int u = 0; u < nu; ++u) {
                int j = m->actuator_trnid[2 * u], v = m->jnt_dofadr[j];
                double ff = 0;
                if (m->jnt_type[j] == mjJNT_BALL)
                    for (int k = 0; k < 3; ++k)
                        ff +=
                            m->actuator_gear[6 * u + k] * (inv->qfrc_inverse[v + k] - feedCorrection[v + k]);
                else
                    ff = inv->qfrc_inverse[v] - feedCorrection[v];
                t.feed[u] = std::clamp(ff, m->actuator_forcerange[2 * u], m->actuator_forcerange[2 * u + 1]);
            }
            for (auto it = settings.importantFrames.begin(); it != settings.importantFrames.end(); ++it)
                if (std::abs(frame - it.key()) < .5f)
                    t.weight += std::max(0.f, it.value());
            if (i == 0 || i == steps)
                t.weight += 5;
            targets[i] = std::move(t);
        }
        beforeF = std::sqrt(beforeF / (steps + 1));
        beforeT = std::sqrt(beforeT / (steps + 1));
        int knots = std::min(8, std::max(2, last - first + 1)), parameters = knots * nu + 1;
        std::vector<double> positionError(nv), lastControl(nu), balance(nv), comJac(3 * nv), feetJac(3 * nv),
            feetRot(3 * nv);
        std::vector<double> limits = b.driveLimits(source), searchScale(nu);
        for (int u = 0; u < nu; ++u)
            searchScale[u] = std::min(limits[u], 50 / std::max(1e-8, m->actuator_acc0[u]));
        auto rollout = [&](const std::vector<double> &parameters, bool save) {
            Rollout out;
            const double gravityBlend = parameters.back();
            mj_resetData(m, d);
            mju_copy(d->qpos, targets.front().q.data(), nq);
            mju_copy(d->qvel, targets.front().v.data(), nv);
            mj_forward(m, d);
            std::fill(lastControl.begin(), lastControl.end(), 0);
            if (save)
                out.q.emplace_back(d->qpos, d->qpos + nq);
            for (int i = 0; i < steps; ++i) {
                need(!control || !control->cancel.load(), "Dynamics optimization cancelled.");
                const auto &t = targets[i];
                mj_differentiatePos(m, positionError.data(), 1, d->qpos, t.q.data());
                double phase = double(i) * (knots - 1) / steps;
                int k0 = std::min(knots - 2, int(phase));
                double mix = phase - k0;
                std::fill(balance.begin(), balance.end(), 0);
                std::vector<int> supportSites;
                float currentFrame = first + float(i) / sub;
                for (const auto &c : schedule)
                    if (contactWeight(c, currentFrame) > 0 && b.indices.contains(c.node))
                        supportSites.push_back(b.bodies[b.indices[c.node]].site);
                if (!supportSites.empty()) {
                    mj_subtreeVel(m, d);
                    mj_jacSubtreeCom(m, d, comJac.data(), 0);
                    QVector3D com(float(d->subtree_com[0]), float(d->subtree_com[1]),
                                  float(d->subtree_com[2]));
                    QVector3D velocity(float(d->subtree_linvel[0]), float(d->subtree_linvel[1]),
                                       float(d->subtree_linvel[2]));
                    auto force = (80 * (t.com - com) + 18 * (t.comVelocity - velocity)) * float(M);
                    // Alternative internal-control initialization: operational-space gravity support.
                    // This adds no root/world force; only the existing internal Jacobian torques below.
                    force += QVector3D(0, float(source.physics.gravity * M * gravityBlend), 0);
                    QVector3D moment;
                    if (rootDofs.size() == 1) {
                        int r = rootDofs.front();
                        int body = m->dof_bodyid[r];
                        QQuaternion rotation(float(d->xquat[4 * body]), float(d->xquat[4 * body + 1]),
                                             float(d->xquat[4 * body + 2]), float(d->xquat[4 * body + 3]));
                        moment = rotation.rotatedVector(
                            {float(1000 * positionError[r + 3] + 150 * (t.v[r + 3] - d->qvel[r + 3])),
                             float(1000 * positionError[r + 4] + 150 * (t.v[r + 4] - d->qvel[r + 4])),
                             float(1000 * positionError[r + 5] + 150 * (t.v[r + 5] - d->qvel[r + 5]))});
                    }
                    for (int site : supportSites) {
                        mj_jacSite(m, d, feetJac.data(), feetRot.data(), site);
                        QVector3D foot(float(d->site_xpos[3 * site]), float(d->site_xpos[3 * site + 1]),
                                       float(d->site_xpos[3 * site + 2]));
                        auto footMoment = moment - QVector3D::crossProduct(foot - com, force);
                        for (int v = 0; v < nv; ++v)
                            for (int k = 0; k < 3; ++k)
                                balance[v] += ((comJac[k * nv + v] - feetJac[k * nv + v]) * force[k] -
                                               feetRot[k * nv + v] * footMoment[k]) /
                                              supportSites.size();
                        const Contact *goal = nullptr;
                        for (const auto &c : schedule)
                            if (b.indices.contains(c.node) && b.bodies[b.indices[c.node]].site == site &&
                                contactWeight(c, currentFrame) > 0) {
                                goal = &c;
                                break;
                            }
                        if (goal) {
                            QVector3D footVelocity;
                            for (int k = 0; k < 3; ++k)
                                for (int v = 0; v < nv; ++v)
                                    footVelocity[k] += float(feetJac[k * nv + v] * d->qvel[v]);
                            auto footForce = (goal->target * b.scale - foot) * 6000 - footVelocity * 250;
                            footForce.setY(0);
                            for (int v = 0; v < nv; ++v)
                                for (int k = 0; k < 3; ++k)
                                    balance[v] += feetJac[k * nv + v] * footForce[k];
                            if (goal->orient) {
                                int body = m->site_bodyid[site];
                                QQuaternion rotation(float(d->xquat[4 * body]), float(d->xquat[4 * body + 1]),
                                                     float(d->xquat[4 * body + 2]),
                                                     float(d->xquat[4 * body + 3]));
                                auto delta = (goal->orientation * rotation.conjugated()).normalized();
                                if (delta.scalar() < 0)
                                    delta = -delta;
                                QVector3D axis;
                                float angle;
                                delta.getAxisAndAngle(&axis, &angle);
                                QVector3D omega;
                                for (int k = 0; k < 3; ++k)
                                    for (int v = 0; v < nv; ++v)
                                        omega[k] += float(feetRot[k * nv + v] * d->qvel[v]);
                                auto torque = axis * qDegreesToRadians(angle) * 1200 - omega * 30;
                                for (int v = 0; v < nv; ++v)
                                    for (int k = 0; k < 3; ++k)
                                        balance[v] += feetRot[k * nv + v] * torque[k];
                            }
                        }
                    }
                }
                for (int u = 0; u < nu; ++u) {
                    int joint = m->actuator_trnid[2 * u], v = m->jnt_dofadr[joint];
                    if (m->jnt_type[joint] == mjJNT_BALL)
                        for (int k = 0; k < 3; ++k)
                            if (m->actuator_gear[6 * u + k] > .5)
                                v += k;
                    int body = m->jnt_bodyid[joint];
                    const RigidSegment *definition = nullptr;
                    for (const auto &bdy : b.bodies)
                        if (bdy.id == body)
                            definition = &bdy.definition;
                    double kp = definition && definition->stiffness >= 0 ? definition->stiffness
                                                                         : source.physics.stiffness,
                           kd = -m->actuator_biasprm[mjNBIAS * u + 2];
                    double feedback = kp * positionError[v] + kd * t.v[v];
                    double offset =
                        (parameters[k0 * nu + u] * (1 - mix) + parameters[(k0 + 1) * nu + u] * mix) *
                        searchScale[u];
                    d->ctrl[u] = (1 - gravityBlend) * t.feed[u] + feedback + offset + balance[v];
                    out.maxTorque =
                        std::max(out.maxTorque,
                                 std::abs(std::clamp(d->ctrl[u] - kd * d->qvel[v], -limits[u], limits[u])));
                    out.cost += settings.smoothWeight * .0001 *
                                std::pow((d->ctrl[u] - lastControl[u]) / std::max(1., limits[u]), 2);
                    lastControl[u] = d->ctrl[u];
                }
                if (save)
                    out.controls.emplace_back(d->ctrl, d->ctrl + nu);
                mj_step(m, d);
                need(d->warning[mjWARN_BADQPOS].number == 0 && d->warning[mjWARN_BADQVEL].number == 0 &&
                         d->warning[mjWARN_BADQACC].number == 0,
                     "MuJoCo reported invalid dynamics; trial rejected without automatic reset.");
                mj_forward(m, d);
                for (int k = 0; k < nv; ++k)
                    need(std::isfinite(d->qvel[k]) && std::abs(d->qvel[k]) < 1e6,
                         "Forward rollout diverged; reduce model stiffness or increase substeps.");
                for (int j = 0; j < m->njnt; ++j)
                    if (m->jnt_type[j] == mjJNT_HINGE && m->jnt_limited[j]) {
                        double value = d->qpos[m->jnt_qposadr[j]];
                        double violation =
                            std::max({0., m->jnt_range[2 * j] - value, value - m->jnt_range[2 * j + 1]});
                        out.limitError = std::max(out.limitError, qRadiansToDegrees(violation));
                        out.cost += 10 * violation * violation;
                    }
                const auto &goal = targets[i + 1];
                mju_copy(inv->qpos, goal.q.data(), nq);
                // Goal cost needs body transforms only, not a second constraint/dynamics solve.
                mj_kinematics(m, inv);
                double goal2 = 0;
                for (const auto &body : b.bodies) {
                    double dx[3];
                    for (int k = 0; k < 3; ++k)
                        dx[k] = d->xpos[3 * body.id + k] - inv->xpos[3 * body.id + k];
                    double e2 = mju_dot3(dx, dx);
                    goal2 += e2;
                    if (goal.weight > 1)
                        out.goalError = std::max(out.goalError, std::sqrt(e2));
                }
                out.cost += settings.poseWeight * goal.weight * goal2 / (H * H * b.bodies.size());
                mj_differentiatePos(m, positionError.data(), 1, d->qpos, goal.q.data());
                double rotation2 = 0;
                for (int k = 0; k < nv; ++k)
                    rotation2 += positionError[k] * positionError[k];
                out.cost += .2 * settings.poseWeight * rotation2 / nv;
                float frame = first + float(i + 1) / sub;
                bool planted = false;
                for (const auto &c : schedule)
                    if (contactWeight(c, frame) > 0) {
                        planted = true;
                        if (!b.indices.contains(c.node))
                            continue;
                        int site = b.bodies[b.indices[c.node]].site;
                        double e2 = 0;
                        for (int k = 0; k < 3; ++k)
                            e2 += std::pow(d->site_xpos[3 * site + k] - c.target[k] * b.scale, 2);
                        double w = contactWeight(c, frame);
                        if (w >= .999 && std::sqrt(e2) > out.contactError) {
                            out.contactError = std::sqrt(e2);
                            out.contactPeakFrame = frame;
                            out.contactPeakNode = c.node;
                        }
                        if (w < .999)
                            out.transitionError = std::max(out.transitionError, std::sqrt(e2));
                        out.cost += settings.contactWeight * w * e2 / (H * H);
                    }
                // Contacts are real unilateral geom contacts; the schedule never becomes a weld.
                for (int c = 0; c < d->ncon; ++c) {
                    auto &contact = d->contact[c];
                    out.penetration = std::max(out.penetration, std::max(0., -contact.dist));
                    double force[6];
                    mj_contactForce(m, d, c, force);
                    out.normalMin = std::min(out.normalMin, force[0]);
                    double tangent = std::hypot(force[1], force[2]),
                           friction = contact.friction[0] * std::max(0., force[0]);
                    out.frictionError = std::max(out.frictionError, std::max(0., tangent - 1.02 * friction));
                    double ratio2 = 0;
                    for (int k = 0; k < 5; ++k)
                        ratio2 += std::pow(
                            force[k + 1] / std::max(1e-12, contact.friction[k] * std::max(0., force[0])), 2);
                    out.wrenchRatio = std::max(out.wrenchRatio, std::sqrt(ratio2));
                    if (!planted && !schedule.empty()) {
                        out.flightForce = std::max(out.flightForce, std::abs(force[0]));
                        out.cost += .01 * std::pow(force[0] / (M * G), 2);
                    }
                    if (save && (i + 1) % sub == 0 && force[0] > .01)
                        out.forces.append(QJsonObject{
                            {"frame", frame},
                            {"positionM", QJsonArray{contact.pos[0], contact.pos[1], contact.pos[2]}},
                            {"normalForceN", force[0]},
                            {"tangentForceN", tangent},
                            {"forceWorldN",
                             QJsonArray{contact.frame[0] * force[0] + contact.frame[3] * force[1] +
                                            contact.frame[6] * force[2],
                                        contact.frame[1] * force[0] + contact.frame[4] * force[1] +
                                            contact.frame[7] * force[2],
                                        contact.frame[2] * force[0] + contact.frame[5] * force[1] +
                                            contact.frame[8] * force[2]}}});
                }
                out.cost += 10 * std::pow(std::max(0., out.penetration - .005 * H) / H, 2);
                if ((i + 1) % sub == 0) {
                    int f = first + (i + 1) / sub;
                    if (f == last || settings.importantFrames.contains(f)) {
                        Scene original = source.motionOnlyCopy();
                        original.evaluate(f);
                        Scene checked = original;
                        b.reconstruct(checked, d);
                        auto p = checked.positions(checked.pose), wanted = original.positions(original.pose);
                        double framePositionError = 0, frameAngleError = 0;
                        for (int node = 0; node < int(checked.nodes.size()); ++node) {
                            framePositionError = std::max(
                                framePositionError, double((p[node] - wanted[node]).length() * b.scale));
                            out.goalError =
                                std::max(out.goalError, double((p[node] - wanted[node]).length() * b.scale));
                            auto delta = original.globalRotation(node, original.pose) *
                                         checked.globalRotation(node, checked.pose).conjugated();
                            double angle = qRadiansToDegrees(
                                2 * std::acos(std::clamp(double(std::abs(delta.scalar())), 0., 1.)));
                            out.goalAngle = std::max(out.goalAngle, angle);
                            frameAngleError = std::max(frameAngleError, angle);
                        }
                        out.cost += steps * (.02 * std::pow(framePositionError / (.02 * H), 2) +
                                             .01 * std::pow(frameAngleError / 5, 2));
                        if (save)
                            out.importantErrors.append(QJsonObject{{"frame", f},
                                                                   {"positionM", framePositionError},
                                                                   {"angleDeg", frameAngleError}});
                        out.limitError = std::max(out.limitError, double(b.limitError(checked)));
                    }
                }
                if (save) {
                    // Check actual forward accelerations using inverse dynamics, without modifying rollout
                    // state.
                    mju_copy(inv->qpos, d->qpos, nq);
                    mju_copy(inv->qvel, d->qvel, nv);
                    mju_copy(inv->qacc, d->qacc, nv);
                    mj_inverse(m, inv);
                    for (int r : rootDofs) {
                        double f2 = 0, t2 = 0;
                        for (int k = 0; k < 3; ++k) {
                            f2 += std::pow(inv->qfrc_inverse[r + k], 2);
                            t2 += std::pow(inv->qfrc_inverse[r + 3 + k], 2);
                        }
                        out.rootForce2 += f2;
                        out.rootTorque2 += t2;
                        out.maxRootForce = std::max(out.maxRootForce, std::sqrt(f2));
                        out.maxRootTorque = std::max(out.maxRootTorque, std::sqrt(t2));
                    }
                    out.q.emplace_back(d->qpos, d->qpos + nq);
                }
            }
            out.cost /= steps;
            out.cost += .1 * std::pow(std::max(0., out.contactError / (.01 * H) - 1), 2) +
                        .1 * std::pow(std::max(0., out.goalError / (.02 * H) - 1), 2);
            mj_subtreeVel(m, d);
            out.endComSpeed = std::sqrt(mju_dot3(d->subtree_linvel, d->subtree_linvel));
            for (const auto &body : b.bodies) {
                mj_jacBodyCom(m, d, feetJac.data(), feetRot.data(), body.id);
                double omega[3]{};
                for (int k = 0; k < 3; ++k)
                    for (int v = 0; v < nv; ++v)
                        omega[k] += feetRot[k * nv + v] * d->qvel[v];
                out.endAngularSpeed = std::max(out.endAngularSpeed, std::sqrt(mju_dot3(omega, omega)));
            }
            if (settings.endAtRest)
                out.cost += 2 * out.endComSpeed * out.endComSpeed / (G * H) +
                            .02 * out.endAngularSpeed * out.endAngularSpeed;
            return out;
        };
        std::vector<double> parameters0(parameters), bestParameters(parameters), direction(parameters),
            plus(parameters), minus(parameters);
        auto preliminaryFeasible = [&](const Rollout &r) {
            return std::isfinite(r.cost) && r.penetration <= .005 * H && r.contactError <= .01 * H &&
                   r.goalError <= .02 * H && r.goalAngle <= 5 && r.limitError <= .5 &&
                   r.frictionError <= 1e-6 * M * G && r.wrenchRatio <= 1.02 && r.flightForce <= .01 * M * G &&
                   (!settings.endAtRest ||
                    (r.endComSpeed <= .01 * std::sqrt(G * H) && r.endAngularSpeed <= .1));
        };
        QStringList failedTrials;
        auto candidate = [&](const std::vector<double> &p) {
            try {
                return rollout(p, false);
            } catch (const std::exception &e) {
                need(!control || !control->cancel.load(), "Dynamics optimization cancelled.");
                if (failedTrials.size() < 8)
                    failedTrials << QString::fromUtf8(e.what());
                Rollout failed;
                failed.cost = std::numeric_limits<double>::infinity();
                return failed;
            }
        };
        auto best = candidate(parameters0);
        const auto initialCandidate = best;
        double initialCost = best.cost;
        bool feasibleFound = preliminaryFeasible(best);
        double feasibleCost = feasibleFound ? best.cost : std::numeric_limits<double>::infinity();
        std::vector<double> feasibleParameters = bestParameters;
        std::mt19937 random(0x54726963);
        int completed = 0;
        QString stop = "iteration budget";
        QJsonArray history;
        history.append(best.cost);
        QJsonArray controlSeeds;
        controlSeeds.append(QJsonObject{
            {"gravitySupportBlend", 0}, {"objective", best.cost}, {"preliminaryFeasible", feasibleFound}});
        if (!feasibleFound && timer.elapsed() < settings.seconds * 1000) {
            auto seed = parameters0;
            seed.back() = 1;
            auto alternative = candidate(seed);
            controlSeeds.append(QJsonObject{{"gravitySupportBlend", 1},
                                            {"objective", alternative.cost},
                                            {"preliminaryFeasible", preliminaryFeasible(alternative)}});
            if (preliminaryFeasible(alternative)) {
                feasibleFound = true;
                feasibleCost = alternative.cost;
                feasibleParameters = seed;
            }
            if (alternative.cost < best.cost) {
                best = alternative;
                bestParameters = seed;
            }
            history.append(best.cost);
        }
        for (int iteration = 0; iteration < settings.iterations; ++iteration) {
            if (timer.elapsed() >= settings.seconds * 1000) {
                stop = "time budget";
                break;
            }
            need(!control || !control->cancel.load(), "Dynamics optimization cancelled.");
            if (control)
                control->progress.store(10 + 80 * iteration / settings.iterations);
            double radius = .02 / std::sqrt(1. + iteration * .2);
            for (int k = 0; k < parameters; ++k) {
                direction[k] = (random() & 1) ? 1 : -1;
                plus[k] = std::clamp(bestParameters[k] + radius * direction[k],
                                     k == parameters - 1 ? 0. : -.5, k == parameters - 1 ? 1. : .5);
                minus[k] = std::clamp(bestParameters[k] - radius * direction[k],
                                      k == parameters - 1 ? 0. : -.5, k == parameters - 1 ? 1. : .5);
            }
            auto positive = candidate(plus), negative = candidate(minus);
            if (preliminaryFeasible(positive) && positive.cost < feasibleCost) {
                feasibleFound = true;
                feasibleCost = positive.cost;
                feasibleParameters = plus;
            }
            if (preliminaryFeasible(negative) && negative.cost < feasibleCost) {
                feasibleFound = true;
                feasibleCost = negative.cost;
                feasibleParameters = minus;
            }
            if (positive.cost < best.cost) {
                best = positive;
                bestParameters = plus;
            }
            if (negative.cost < best.cost) {
                best = negative;
                bestParameters = minus;
            }
            history.append(best.cost);
            completed = iteration + 1;
        }
        need(std::isfinite(best.cost),
             "No finite bounded rollout found in the budget. Inspect physical model, gains and timestep; "
             "mathematical infeasibility is not established.");
        auto final = rollout(feasibleFound ? feasibleParameters : bestParameters, true);
        // Independent replay of the saved admissible controls. qpos is set once at t0 only.
        auto replay = b.data();
        mju_copy(replay->qpos, targets.front().q.data(), nq);
        mju_copy(replay->qvel, targets.front().v.data(), nv);
        mj_forward(m, replay.get());
        double replay2 = 0, replayPeak = 0;
        int samples = 0;
        for (int i = 0; i < steps; ++i) {
            mju_copy(replay->ctrl, final.controls[i].data(), nu);
            mj_step(m, replay.get());
            mj_forward(m, replay.get());
            if ((i + 1) % sub == 0) {
                mj_differentiatePos(m, positionError.data(), 1, replay->qpos, final.q[i + 1].data());
                for (double e : positionError) {
                    replay2 += e * e;
                    replayPeak = std::max(replayPeak, std::abs(e));
                    ++samples;
                }
            }
        }
        int ratio = 1;
        if (settings.outputFps == 0) {
            for (int k = 1; k <= std::min(sub, 240 / source.fps); ++k)
                if (sub % k == 0)
                    ratio = k;
        } else {
            need(settings.outputFps >= source.fps && settings.outputFps <= 240 &&
                     settings.outputFps % source.fps == 0,
                 "Output FPS must be a source-rate multiple up to 240.");
            ratio = settings.outputFps / source.fps;
            need(sub % ratio == 0, "Output FPS must divide the native simulation rate.");
        }
        Scene out = physicsBake(source, control, source.fps * ratio);
        out.bodies = mj::definitions(source);
        Scene artistic = source.motionOnlyCopy(), comparison = source.motionOnlyCopy();
        QJsonArray trajectories;
        double importantAngle = 0;

        for (int f = first * ratio; f <= last * ratio; ++f) {
            out.evaluate(f);
            int native = (f - first * ratio) * (sub / ratio);
            mju_copy(d->qpos, final.q[native].data(), nq);
            mj_forward(m, d);
            b.reconstruct(out, d);
            final.limitError = std::max(final.limitError, double(b.limitError(out)));
            out.keys[f] = {out.pose, {}, out.morphs};
            comparison.evaluate(float(f) / ratio);
            auto sourcePositions = comparison.positions(comparison.pose),
                 resultPositions = out.positions(out.pose);
            QJsonObject sourceEffectors, resultEffectors;
            auto vec = [](QVector3D v) { return QJsonArray{v.x(), v.y(), v.z()}; };
            for (const auto &body : b.bodies) {
                auto id = QString::number(body.definition.node);
                sourceEffectors[id] = vec(sourcePositions[body.definition.node] * b.scale);
                resultEffectors[id] = vec(resultPositions[body.definition.node] * b.scale);
            }
            trajectories.append(QJsonObject{{"frame", f},
                                            {"sourceComM", vec(physicalCenterOfMass(comparison) * b.scale)},
                                            {"resultComM", vec(physicalCenterOfMass(out) * b.scale)},
                                            {"sourceEffectorsM", sourceEffectors},
                                            {"resultEffectorsM", resultEffectors}});
            int sourceFrame = f / ratio;
            if (f % ratio == 0 && (sourceFrame == first || sourceFrame == last ||
                                   settings.importantFrames.contains(sourceFrame))) {
                artistic.evaluate(sourceFrame);
                auto a = artistic.positions(artistic.pose), p = out.positions(out.pose);
                for (int n = 0; n < int(out.nodes.size()); ++n) {
                    final.goalError = std::max(final.goalError, double((a[n] - p[n]).length() * b.scale));
                    auto delta = artistic.globalRotation(n, artistic.pose) *
                                 out.globalRotation(n, out.pose).conjugated();
                    importantAngle =
                        std::max(importantAngle,
                                 qRadiansToDegrees(
                                     2 * std::acos(std::clamp(double(std::abs(delta.scalar())), 0., 1.))));
                }
            }
        }
        const auto representation =
            bakePhysicsTrajectory(source, b, final.q, first, last, out, true, control, targets.front().v);
        const auto bakedKinematics = comparePhysicsTrajectory(out, b, final.q, first, last, control);
        const auto boundaries = physicsIntervalBoundaries(source, out, b, first, last);
        double rootF = std::sqrt(final.rootForce2 / steps), rootT = std::sqrt(final.rootTorque2 / steps);
        bool feasible = rootF <= .03 * M * G && rootT <= .03 * M * G * H &&
                        final.maxRootForce <= .1 * M * G && final.maxRootTorque <= .1 * M * G * H &&
                        final.penetration <= .005 * H && final.contactError <= .01 * H &&
                        final.goalError <= .02 * H && importantAngle <= 5 && final.limitError <= .5 &&
                        final.frictionError <= 1e-6 * M * G && final.wrenchRatio <= 1.02 &&
                        final.flightForce <= .01 * M * G &&
                        (!settings.endAtRest ||
                         (final.endComSpeed <= .01 * std::sqrt(G * H) && final.endAngularSpeed <= .1));
        report = {};
        report.status = feasible ? "solved" : "approximate";
        report.stopReason = stop;
        report.frames = (last - first) * ratio + 1;
        report.bodies = int(b.bodies.size());
        report.dofs = nv;
        report.iterations = completed;
        report.totalMass = float(M);
        report.elapsedMs = timer.elapsed();
        report.rootForceRms = float(rootF);
        report.rootTorqueRms = float(rootT);
        report.maxTorque = float(final.maxTorque);
        report.maxContactError = float(final.contactError);
        report.maxPenetration = float(final.penetration);
        report.maxTargetPosition = float(final.goalError);
        report.maxTargetAngle = float(importantAngle);
        report.maxLimitError = float(final.limitError);
        report.details = {
            {"algorithm", "bounded control-knot shooting; symmetric deterministic perturbation "
                          "search + inverse feedforward and internal PD"},
            {"controllerMethodVersion", "bounded-support-seed-shooting/2"},
            {"controlSeeds", controlSeeds},
            {"failedTrialReasons", QJsonArray::fromStringList(failedTrials)},
            {"gravitySupportBlend", (feasibleFound ? feasibleParameters : bestParameters).back()},
            {"calculationNumerics", QJsonObject{{"integrator", "implicitfast"},
                                                {"timestepSeconds", dt},
                                                {"sourceFps", source.fps},
                                                {"substepsPerSourceFrame", sub}}},
            {"initialObjective", initialCost},
            {"finalObjective", final.cost},
            {"objectiveHistory", history},
            {"nativeInverseRootForceRmsN", rootF},
            {"nativeInverseRootTorqueRmsNm", rootT},
            {"rootForcePeakN", final.maxRootForce},
            {"rootTorquePeakNm", final.maxRootTorque},
            {"forwardReplayTangentRms", std::sqrt(replay2 / std::max(1, samples))},
            {"forwardReplayTangentPeak", replayPeak},
            {"importantPositionErrorM", final.goalError},
            {"importantAngleErrorDeg", importantAngle},
            {"minimumNormalForceN", final.normalMin},
            {"frictionViolationN", final.frictionError},
            {"contactWrenchConeRatio", final.wrenchRatio},
            {"transitionPositionErrorM", final.transitionError},
            {"bestFeasibleFound", feasibleFound},
            {"unexpectedFlightContactN", final.flightForce},
            {"externalAnchors", false},
            {"rootActuators", 0},
            {"substepSeconds", dt},
            {"outputFps", out.fps},
            {"controlKnots", knots},
            {"referenceSmoothingLambda", smoothing},
            {"endAtRest", settings.endAtRest},
            {"prepareLanding", settings.prepareLanding},
            {"endHoldFrames", endHold},
            {"endComSpeedMS", final.endComSpeed},
            {"endAngularSpeedRadS", final.endAngularSpeed},
            {"optimizationIterations", completed},
            {"timeBudgetSeconds", settings.seconds},
            {"supportComKp", 80},
            {"supportComKd", 18},
            {"supportOrientationKp", 1000},
            {"supportOrientationKd", 150},
            {"supportFootKp", 6000},
            {"supportFootKd", 250},
            {"supportFootOrientationKp", 1200},
            {"supportFootOrientationKd", 30},
            {"contactForces", final.forces},
            {"forwardVerified", feasible}};
        if (!feasible)
            report.warnings
                << "No solution certified within the specified budget: inspect contact, root residual, "
                   "pose and limit metrics. Best bounded forward rollout retained.";
        auto beforeSampled = sampledMotionInverse(b, source, double(first) / source.fps,
                                                  double(last) / source.fps, source.fps, control);
        auto afterSampled = sampledMotionInverse(b, out, double(first) / source.fps,
                                                 double(last) / source.fps, source.fps, control);
        report.details["sampledAnimationInverse"] =
            QJsonObject{{"before", beforeSampled}, {"after", afterSampled}};
        report.details["referenceRootForceRmsN"] = beforeF;
        report.details["referenceRootTorqueRmsNm"] = beforeT;
        report.details["beforeRootForceRmsN"] = beforeSampled["rootForceRmsN"].toDouble();
        report.details["beforeRootTorqueRmsNm"] = beforeSampled["rootTorqueRmsNm"].toDouble();
        report.details["afterRootForceRmsN"] = afterSampled["rootForceRmsN"];
        report.details["afterRootTorqueRmsNm"] = afterSampled["rootTorqueRmsNm"];
        report.details["reportSchemaVersion"] = 2;
        report.details["legacyRootResidualAliases"] =
            "Deprecated: before/afterRoot* now both alias sampledAnimationInverse (centered-pose-inverse/1). "
            "Reports without reportSchemaVersion=2 mixed native after with sampled before; never compare "
            "those flat fields.";
        bool bakedVerified = inverseResidualPassed(afterSampled, M, G, H);
        report.details["nativeForwardVerified"] = feasible;
        report.details["bakedInverseVerified"] = bakedVerified;
        report.details["validationScope"] = "Native control replay and geometric samples; baked pose "
                                            "derivatives have an additional inverse-dynamics gate.";
        const double sampledBefore = beforeSampled["rootForceRmsN"].toDouble();
        report.details["sampledRootForceReduction"] =
            sampledBefore > 1e-9 ? QJsonValue(1 - afterSampled["rootForceRmsN"].toDouble() / sampledBefore)
                                 : QJsonValue(QJsonValue::Null);
        if (sampledBefore <= 1e-9)
            report.details["sampledReductionReason"] =
                "Source residual is below numerical floor; percentage improvement is undefined";
        if (!bakedVerified && report.status == "solved") {
            report.status = "approximate";
            report.stopReason += "; native forward passed, baked inverse residual exceeds tolerance";
            report.warnings << "Native forward replay is valid, but inverse dynamics of the baked keys "
                               "exceeds the fixed root-force/moment tolerance. The baked result is "
                               "approximate; it is not a complete physical certificate.";
        }
        if (!bakedKinematics["passed"].toBool()) {
            report.status = "approximate";
            report.warnings << "Baked evaluator differs from native trajectory beyond kinematic tolerances.";
        }
        report.details["intervalBoundaries"] = boundaries;
        if (!boundaries["compatible"].toBool()) {
            report.status = "approximate";
            report.warnings
                << "Interval boundary velocity/pose conflict: inspect the measured one-sided residuals.";
        }
        report.details["trajectoryRepresentation"] = representation;
        report.details["contactPreparation"] = preparation;
        report.details["trajectory"] = trajectories;
        report.details["importantFramesMeasured"] = final.importantErrors;
        report.details["contactPeakFrame"] = final.contactPeakFrame;
        report.details["contactPeakNode"] = final.contactPeakNode;
        recordPhysicsProvenance(source, first, last, report, b.xml);
        recordControlReplay(report, b.xml, targets.front().q, targets.front().v, final.q, final.controls);
        QJsonObject independent;
        QString replayError;
        if (!verifyControlReplay(report.json(), independent, replayError)) {
            report.warnings << "Independent control replay: " + replayError;
            report.status = "approximate";
            report.details["forwardVerified"] = false;
            report.details["nativeForwardVerified"] = false;
            report.stopReason += "; independent impulse verification failed";
        }
        report.details["independentReplay"] = independent;
        QJsonObject nativeConsistency{{"methodVersion", "mujoco-forward-inverse/1"},
                                      {"analysisFps", 1 / dt},
                                      {"rootForceRmsN", rootF},
                                      {"rootTorqueRmsNm", rootT},
                                      {"rootForcePeakN", final.maxRootForce},
                                      {"rootTorquePeakNm", final.maxRootTorque},
                                      {"units", "N, Nm"},
                                      {"scope", "native acceleration and native contacts only"}};
        auto candidateMetrics = [&](const Rollout &r) {
            return QJsonObject{{"objective", r.cost},
                               {"plantedErrorM", r.contactError},
                               {"penetrationM", r.penetration},
                               {"importantPositionErrorM", r.goalError},
                               {"importantAngleErrorDeg", r.goalAngle},
                               {"directedLimitErrorDeg", r.limitError},
                               {"endComSpeedMS", r.endComSpeed},
                               {"endAngularSpeedRadS", r.endAngularSpeed},
                               {"preliminaryFeasible", preliminaryFeasible(r)}};
        };
        report.details["candidateComparison"] =
            QJsonObject{{"methodVersion", "same-forward-objective/1"},
                        {"before", candidateMetrics(initialCandidate)},
                        {"after", candidateMetrics(final)},
                        {"scope", "Initial and found bounded controls, same native model, integrator, "
                                  "contact schedule and objective"}};
        report.details["measurements"] =
            QJsonObject{{"nativeConsistency", nativeConsistency},
                        {"nativeImpulseReplay", independent},
                        {"bakedDynamics", QJsonObject{{"before", beforeSampled}, {"after", afterSampled}}},
                        {"bakedKinematics", bakedKinematics},
                        {"optimizerObjective", QJsonObject{{"methodVersion", "bounded-shooting-objective/1"},
                                                           {"before", initialCost},
                                                           {"after", final.cost},
                                                           {"units", "dimensionless"}}}};
        report.details["validationDomains"] = QJsonObject{
            {"nativeControl",
             QJsonObject{{"status", report.details["nativeForwardVerified"].toBool() ? "passed" : "failed"},
                         {"methodVersion", "native-replay-impulse/1"}}},
            {"bakedKinematics",
             QJsonObject{{"status", bakedKinematics["passed"].toBool() ? "passed" : "failed"},
                         {"methodVersion", "native-evaluator-kinematics/1"}}},
            {"bakedDynamics",
             QJsonObject{{"status", bakedVerified ? "passed" : "failed"},
                         {"methodVersion", "centered-pose-inverse/1"},
                         {"analysisFps", source.fps},
                         {"reason", bakedVerified ? "Inverse residual thresholds met on this grid"
                                                  : "Inverse residual thresholds exceeded"}}},
            {"exportKinematics",
             QJsonObject{{"status", "not_checked"}, {"reason", "Independent consumer required"}}},
            {"exportDynamics",
             QJsonObject{{"status", "not_checked"}, {"reason", "Actual imported trajectory required"}}}};
        Scene beforeCurve = source.motionOnlyCopy(), afterCurve = out.motionOnlyCopy();
        beforeCurve.compareOriginal = afterCurve.compareOriginal = false;
        auto configuration = [&](Scene &scene, double time, double *q) {
            scene.evaluate(float(time * scene.fps));
            need(b.pose(scene, q, error), error);
        };
        const double beginSeconds = double(first) / source.fps, endSeconds = double(last) / source.fps;
        auto commands = [&](double time, double *u) {
            int i = std::clamp(int((time - beginSeconds) / dt + 1e-8), 0, steps - 1);
            mju_copy(u, final.controls[i].data(), nu);
        };
        auto beforeImpulse = motionImpulseBalance(
            b, [&](double t, double *q) { configuration(beforeCurve, t, q); }, commands, beginSeconds,
            endSeconds, source.fps, .001, control);
        auto afterImpulse = motionImpulseBalance(
            b, [&](double t, double *q) { configuration(afterCurve, t, q); }, commands, beginSeconds,
            endSeconds, source.fps, .001, control);
        bool impulsePassed = inverseResidualPassed(afterImpulse, M, G, H) &&
                             afterImpulse["generalizedRootForceRmsN"].toDouble() <= .03 * M * G &&
                             afterImpulse["generalizedRootForcePeakN"].toDouble() <= .1 * M * G &&
                             afterImpulse["generalizedRootMomentRmsNm"].toDouble() <= .03 * M * G * H &&
                             afterImpulse["generalizedRootMomentPeakNm"].toDouble() <= .1 * M * G * H &&
                             afterImpulse["requiredWindowTorqueLimitRatio"].toDouble() <= 1.01 &&
                             afterImpulse["actualTorqueLimitRatio"].toDouble() <= 1.01 &&
                             afterImpulse["penetrationM"].toDouble() <= .005 * H;
        auto measurements = report.details["measurements"].toObject();
        measurements["bakedImpulse"] = QJsonObject{
            {"before", beforeImpulse},
            {"after", afterImpulse},
            {"comparison",
             "Same frozen model, found bounded controls, quadrature, derivative probes and window grid"}};
        report.details["measurements"] = measurements;
        auto domains = report.details["validationDomains"].toObject();
        domains["bakedImpulseBalance"] =
            QJsonObject{{"status", impulsePassed ? "passed" : "failed"},
                        {"methodVersion", "represented-forward-impulse/2"},
                        {"reason", "Windowed momentum/generalized balance, torque bounds and penetration "
                                   "only; point acceleration and artistic goals have separate gates"}};
        domains["intervalBoundaries"] =
            QJsonObject{{"status", boundaries["compatible"].toBool() ? "passed" : "conflict"},
                        {"methodVersion", "interval-one-sided-continuity/1"}};
        report.details["validationDomains"] = domains;
        if (!impulsePassed) {
            report.status = "approximate";
            report.warnings << "Represented-motion impulse/drive validation exceeds tolerance; inspect the "
                               "separate baked measurement.";
        }
        if (control)
            control->progress.store(97);
        QJsonArray goals;
        for (auto it = settings.importantFrames.begin(); it != settings.importantFrames.end(); ++it)
            goals.append(QJsonObject{{"frame", it.key() * ratio}, {"weight", it.value()}});
        out.physics.requests["optimize"] = QJsonObject{{"iterations", settings.iterations},
                                                       {"seconds", settings.seconds},
                                                       {"importantFrames", goals},
                                                       {"endAtRest", settings.endAtRest},
                                                       {"prepareLanding", settings.prepareLanding},
                                                       {"outputFps", 0},
                                                       {"poseWeight", settings.poseWeight},
                                                       {"contactWeight", settings.contactWeight},
                                                       {"smoothWeight", settings.smoothWeight}};
        report.elapsedMs = timer.elapsed();
        out.physicsResult = report.json();
        out.name = source.name + " · dynamics correction (" + report.status + ")";
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
