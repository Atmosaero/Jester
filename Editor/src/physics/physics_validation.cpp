#include "core/scene/runtime_error.h"
#include "physics/physics_validation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace trickster {
namespace {
void need(bool ok, const QString &error) {
    if (!ok)
        throw trickster::RuntimeError(error.toStdString());
}
} // namespace
QJsonObject sampledConfigurationInverse(mj::Backend &backend, const ConfigurationSampler &sample,
                                        double firstSeconds, double lastSeconds, int analysisFps,
                                        PhysicsControl *control, bool timeSeries) {
    auto m = backend.model.get();
    auto d = backend.data();
    QJsonArray series;
    std::vector<double> qm(m->nq), qp(m->nq), vm(m->nv), vp(m->nv);
    double f2 = 0, t2 = 0, peakF = 0, peakT = 0;
    int count = 0;
    const double dt = 1. / analysisFps;
    need(analysisFps >= 1 && analysisFps <= 20000 && lastSeconds > firstSeconds,
         "Invalid inverse analysis grid.");
    for (double time = firstSeconds + dt; time < lastSeconds - dt * .5; time += dt) {
        need(!control || !control->cancel.load(), "Dynamics analysis cancelled.");
        sample(time - dt, qm.data());
        sample(time, d->qpos);
        sample(time + dt, qp.data());
        mj_differentiatePos(m, vm.data(), dt, qm.data(), d->qpos);
        mj_differentiatePos(m, vp.data(), dt, d->qpos, qp.data());
        for (int k = 0; k < m->nv; ++k) {
            d->qvel[k] = (vm[k] + vp[k]) * .5;
            d->qacc[k] = (vp[k] - vm[k]) / dt;
        }
        mj_inverse(m, d.get());
        double force = 0, torque = 0;
        for (int j = 0; j < m->njnt; ++j)
            if (m->jnt_type[j] == mjJNT_FREE) {
                int v = m->jnt_dofadr[j];
                for (int k = 0; k < 3; ++k) {
                    force += std::pow(d->qfrc_inverse[v + k], 2);
                    torque += std::pow(d->qfrc_inverse[v + 3 + k], 2);
                }
            }
        f2 += force;
        t2 += torque;
        peakF = std::max(peakF, std::sqrt(force));
        peakT = std::max(peakT, std::sqrt(torque));
        if (timeSeries)
            series.append(QJsonObject{{"seconds", time},
                                      {"rootForceN", std::sqrt(force)},
                                      {"rootMomentNm", std::sqrt(torque)},
                                      {"contacts", d->ncon}});
        ++count;
    }
    return {{"timeSeries", series},
            {"rootForceRmsN", std::sqrt(f2 / std::max(1, count))},
            {"rootTorqueRmsNm", std::sqrt(t2 / std::max(1, count))},
            {"rootForcePeakN", peakF},
            {"rootTorquePeakNm", peakT},
            {"analysisFps", analysisFps},
            {"samples", count},
            {"methodVersion", "centered-pose-inverse/1"},
            {"units", "m, kg, s, N, Nm"},
            {"scope", "evaluated animation, including interpolation; not native controls"},
            {"firstSeconds", firstSeconds},
            {"lastSeconds", lastSeconds},
            {"method", "common centered pose differences on the specified time grid; boundaries excluded; "
                       "contact forces recomputed by inverse dynamics; not a forward certificate"}};
}
QJsonObject sampledMotionInverse(mj::Backend &backend, const Scene &scene, double firstSeconds,
                                 double lastSeconds, int analysisFps, PhysicsControl *control) {
    Scene state = scene.motionOnlyCopy();
    state.compareOriginal = false;
    auto sample = [&](double time, double *q) {
        state.evaluate(float(time * scene.fps));
        QString error;
        need(backend.pose(state, q, error), error);
    };
    return sampledConfigurationInverse(backend, sample, firstSeconds, lastSeconds, analysisFps, control);
}

namespace {
struct WrenchSample {
    double p[3]{}, l[3]{}, force[3]{}, moment[3]{};
    std::vector<double> mass, velocity, rhs, actuator;
    double torqueRatio = 0, angularSpeed = 0, massKg = 0, penetration = 0;
};
WrenchSample representedWrench(const mjModel *m, mjData *d) {
    WrenchSample out;
    out.mass.resize(size_t(m->nv) * m->nv);
    mj_fullM(m, out.mass.data(), d->qM);
    out.velocity.assign(d->qvel, d->qvel + m->nv);
    out.rhs.resize(m->nv);
    out.actuator.assign(d->qfrc_actuator, d->qfrc_actuator + m->nv);
    for (int u = 0; u < m->nu; u++) {
        double bound =
            std::max(std::abs(m->actuator_forcerange[2 * u]), std::abs(m->actuator_forcerange[2 * u + 1]));
        out.torqueRatio = std::max(out.torqueRatio, std::abs(d->actuator_force[u]) / std::max(1e-12, bound));
    }
    for (int c = 0; c < d->ncon; c++)
        out.penetration = std::max(out.penetration, -d->contact[c].dist);
    for (int k = 0; k < m->nv; k++)
        out.rhs[k] = d->qfrc_smooth[k] + d->qfrc_constraint[k];
    std::vector<double> jp(3 * m->nv), jr(3 * m->nv);
    for (int body = 1; body < m->nbody; ++body) {
        mj_jacBodyCom(m, d, jp.data(), jr.data(), body);
        double vel[3]{}, omega[3]{}, linear[3]{}, spin[3]{}, orbital[3]{}, gravity[3]{};
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < m->nv; j++) {
                vel[k] += jp[k * m->nv + j] * d->qvel[j];
                omega[k] += jr[k * m->nv + j] * d->qvel[j];
            }
        out.massKg += m->body_mass[body];
        out.angularSpeed = std::max(out.angularSpeed, mju_norm3(omega));
        const double *R = d->ximat + 9 * body;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                for (int k = 0; k < 3; k++)
                    spin[i] += R[3 * i + k] * m->body_inertia[3 * body + k] * R[3 * j + k] * omega[j];
        for (int k = 0; k < 3; k++) {
            linear[k] = m->body_mass[body] * vel[k];
            gravity[k] = m->body_mass[body] * m->opt.gravity[k];
            out.p[k] += linear[k];
            out.force[k] += gravity[k];
        }
        mju_cross(orbital, d->xipos + 3 * body, linear);
        double arm[3];
        mju_cross(arm, d->xipos + 3 * body, gravity);
        for (int k = 0; k < 3; k++) {
            out.l[k] += orbital[k] + spin[k];
            out.moment[k] += arm[k];
        }
    }
    for (int c = 0; c < d->ncon; c++) {
        const auto &contact = d->contact[c];
        int a = m->geom_bodyid[contact.geom[0]], b = m->geom_bodyid[contact.geom[1]];
        if ((a == 0) == (b == 0))
            continue;
        double local[6], f[3]{}, t[3]{}, arm[3];
        mj_contactForce(m, d, c, local);
        double sign = a == 0 ? 1 : -1;
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 3; j++) {
                f[k] += sign * contact.frame[3 * j + k] * local[j];
                t[k] += sign * contact.frame[3 * j + k] * local[j + 3];
            }
        mju_cross(arm, contact.pos, f);
        for (int k = 0; k < 3; k++) {
            out.force[k] += f[k];
            out.moment[k] += arm[k] + t[k];
        }
    }
    return out;
}
} // namespace
QJsonObject motionImpulseBalance(mj::Backend &backend, const ConfigurationSampler &sample,
                                 const ControlSampler &controls, double firstSeconds, double lastSeconds,
                                 int analysisFps, double derivativeHalfWidth, PhysicsControl *control) {
    auto m = backend.model.get();
    auto d = backend.data();
    need(m->neq == 0 && m->nmocap == 0, "Represented dynamics cannot certify external anchors.");
    need(analysisFps > 0 && analysisFps <= 240 && lastSeconds > firstSeconds && derivativeHalfWidth > 0 &&
             derivativeHalfWidth < .1,
         "Invalid impulse analysis grid.");
    std::vector<double> qm(m->nq), qp(m->nq);
    auto evaluate = [&](double time) {
        need(!control || !control->cancel.load(), "Impulse analysis cancelled.");
        double lo = std::max(firstSeconds, time - derivativeHalfWidth),
               hi = std::min(lastSeconds, time + derivativeHalfWidth);
        sample(lo, qm.data());
        sample(hi, qp.data());
        sample(time, d->qpos);
        mj_differentiatePos(m, d->qvel, hi - lo, qm.data(), qp.data());
        mju_zero(d->ctrl, m->nu);
        if (controls)
            controls(time, d->ctrl);
        mj_forward(m, d.get());
        return representedWrench(m, d.get());
    };
    double f2 = 0, t2 = 0, peakF = 0, peakT = 0, forceImpulse[3]{}, momentImpulse[3]{};
    int windows = 0;
    QJsonArray series;
    std::vector<double> generalizedImpulse(m->nv), actuatorImpulse(m->nv), torqueBounds(m->nv);
    for (int u = 0; u < m->nu; u++) {
        int j = m->actuator_trnid[2 * u], v = m->jnt_dofadr[j];
        need(m->jnt_type[j] != mjJNT_FREE, "A free-root actuator is forbidden.");
        double bound =
            std::max(std::abs(m->actuator_forcerange[2 * u]), std::abs(m->actuator_forcerange[2 * u + 1]));
        int count = m->jnt_type[j] == mjJNT_BALL ? 3 : 1;
        for (int k = 0; k < count; k++)
            torqueBounds[v + k] += bound * std::abs(m->actuator_gear[6 * u + k]);
    }
    double requiredTorqueRatio = 0, actualTorqueRatio = 0, penetration = 0;
    double generalizedForce2 = 0, generalizedMoment2 = 0, generalizedForcePeak = 0, generalizedMomentPeak = 0,
           jointResidualPeak = 0;
    double windowStart = firstSeconds;
    auto start = evaluate(firstSeconds), previous = start;
    const int steps = int(std::ceil((lastSeconds - firstSeconds) / m->opt.timestep));
    double previousTime = firstSeconds;
    for (int i = 1; i <= steps; i++) {
        double time = std::min(lastSeconds, firstSeconds + i * m->opt.timestep);
        auto current = evaluate(time);
        double h = time - previousTime;
        actualTorqueRatio = std::max({actualTorqueRatio, previous.torqueRatio, current.torqueRatio});
        penetration = std::max({penetration, previous.penetration, current.penetration});
        for (int k = 0; k < 3; k++) {
            forceImpulse[k] += .5 * h * (previous.force[k] + current.force[k]);
            momentImpulse[k] += .5 * h * (previous.moment[k] + current.moment[k]);
        }
        for (int j = 0; j < m->nv; j++) {
            for (int k = 0; k < m->nv; k++)
                generalizedImpulse[j] += .5 * (previous.mass[j * m->nv + k] + current.mass[j * m->nv + k]) *
                                         (current.velocity[k] - previous.velocity[k]);
            generalizedImpulse[j] -= .5 * h * (previous.rhs[j] + current.rhs[j]);
            actuatorImpulse[j] += .5 * h * (previous.actuator[j] + current.actuator[j]);
        }
        if (time + 1e-10 >= std::min(lastSeconds, firstSeconds + double(windows + 1) / analysisFps)) {
            double force[3], moment[3];
            for (int k = 0; k < 3; k++) {
                force[k] = (current.p[k] - start.p[k] - forceImpulse[k]) / (time - windowStart);
                moment[k] = (current.l[k] - start.l[k] - momentImpulse[k]) / (time - windowStart);
                forceImpulse[k] = momentImpulse[k] = 0;
            }
            double generalizedF = 0, generalizedT = 0;
            std::vector<bool> root(m->nv);
            for (int j = 0; j < m->njnt; j++)
                if (m->jnt_type[j] == mjJNT_FREE) {
                    int v = m->jnt_dofadr[j];
                    for (int k = 0; k < 6; k++)
                        root[v + k] = true;
                    for (int k = 0; k < 3; k++) {
                        generalizedF += std::pow(generalizedImpulse[v + k] / (time - windowStart), 2);
                        generalizedT += std::pow(generalizedImpulse[v + 3 + k] / (time - windowStart), 2);
                    }
                }
            for (int j = 0; j < m->nv; j++)
                if (!root[j])
                    jointResidualPeak =
                        std::max(jointResidualPeak, std::abs(generalizedImpulse[j] / (time - windowStart)));
            generalizedForce2 += generalizedF;
            generalizedMoment2 += generalizedT;
            generalizedForcePeak = std::max(generalizedForcePeak, std::sqrt(generalizedF));
            generalizedMomentPeak = std::max(generalizedMomentPeak, std::sqrt(generalizedT));
            for (int j = 0; j < m->nv; j++)
                if (!root[j] && torqueBounds[j] > 0)
                    requiredTorqueRatio =
                        std::max(requiredTorqueRatio, std::abs(generalizedImpulse[j] + actuatorImpulse[j]) /
                                                          ((time - windowStart) * torqueBounds[j]));
            std::fill(generalizedImpulse.begin(), generalizedImpulse.end(), 0.);
            std::fill(actuatorImpulse.begin(), actuatorImpulse.end(), 0.);
            double f = std::sqrt(mju_dot3(force, force)), t = std::sqrt(mju_dot3(moment, moment));
            f2 += f * f;
            t2 += t * t;
            peakF = std::max(peakF, f);
            peakT = std::max(peakT, t);
            series.append(QJsonObject{
                {"seconds", time}, {"rootForceN", f}, {"rootMomentNm", t}, {"contacts", d->ncon}});
            ++windows;
            start = current;
            windowStart = time;
        }
        previous = current;
        previousTime = time;
    }
    return QJsonObject{{"methodVersion", "represented-forward-impulse/2"},
                       {"scope", "Actual represented q(t); differentiated poses, bounded internal controls "
                                 "and recomputed forward ground forces. No saved qvel/qacc."},
                       {"analysisFps", analysisFps},
                       {"quadratureStepSeconds", m->opt.timestep},
                       {"derivativeHalfWidthSeconds", derivativeHalfWidth},
                       {"units", "N, Nm"},
                       {"angularMomentumReference", "world origin; orbital plus spin"},
                       {"samples", windows},
                       {"rootForceRmsN", std::sqrt(f2 / std::max(1, windows))},
                       {"rootTorqueRmsNm", std::sqrt(t2 / std::max(1, windows))},
                       {"rootForcePeakN", peakF},
                       {"rootTorquePeakNm", peakT},
                       {"generalizedRootForceRmsN", std::sqrt(generalizedForce2 / std::max(1, windows))},
                       {"generalizedRootMomentRmsNm", std::sqrt(generalizedMoment2 / std::max(1, windows))},
                       {"generalizedRootForcePeakN", generalizedForcePeak},
                       {"generalizedRootMomentPeakNm", generalizedMomentPeak},
                       {"jointTorqueResidualPeakNm", jointResidualPeak},
                       {"requiredWindowTorqueLimitRatio", requiredTorqueRatio},
                       {"actualTorqueLimitRatio", actualTorqueRatio},
                       {"penetrationM", penetration},
                       {"endComSpeedMS", mju_norm3(previous.p) / std::max(1e-12, previous.massKg)},
                       {"endAngularSpeedRadS", previous.angularSpeed},
                       {"timeSeries", series}};
}
bool inverseResidualPassed(const QJsonObject &a, double mass, double gravity, double height) {
    const double force = mass * std::max(1., gravity), torque = force * height;
    for (auto key : {"rootForceRmsN", "rootForcePeakN", "rootTorqueRmsNm", "rootTorquePeakNm"})
        if (!a[key].isDouble() || !std::isfinite(a[key].toDouble()))
            return false;
    return a["samples"].toInt() > 0 && a["rootForceRmsN"].toDouble() <= .03 * force &&
           a["rootTorqueRmsNm"].toDouble() <= .03 * torque && a["rootForcePeakN"].toDouble() <= .1 * force &&
           a["rootTorquePeakNm"].toDouble() <= .1 * torque;
}
} // namespace trickster
