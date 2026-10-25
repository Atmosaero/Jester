#include "core/scene/runtime_error.h"
#include "physics/physics_replay.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mujoco/mujoco.h>
#include <stdexcept>
#include <vector>
namespace trickster {
namespace {
void need(bool ok, const char *message) {
    if (!ok)
        throw trickster::RuntimeError(message);
}
struct Momentum {
    double p[3]{}, l[3]{}, com[3]{}, mass = 0;
};
Momentum momentum(const mjModel *m, mjData *d) {
    Momentum out;
    std::vector<double> jp(3 * m->nv), jr(3 * m->nv);
    for (int b = 1; b < m->nbody; ++b) {
        mj_jacBodyCom(m, d, jp.data(), jr.data(), b);
        double v[3]{}, omega[3]{}, spin[3]{}, linear[3]{}, orbital[3]{};
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < m->nv; ++j) {
                v[k] += jp[k * m->nv + j] * d->qvel[j];
                omega[k] += jr[k * m->nv + j] * d->qvel[j];
            }
        const double *R = d->ximat + 9 * b;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    spin[i] += R[3 * i + k] * m->body_inertia[3 * b + k] * R[3 * j + k] * omega[j];
        for (int k = 0; k < 3; ++k) {
            linear[k] = m->body_mass[b] * v[k];
            out.p[k] += linear[k];
            out.com[k] += m->body_mass[b] * d->xipos[3 * b + k];
        }
        mju_cross(orbital, d->xipos + 3 * b, linear);
        for (int k = 0; k < 3; ++k)
            out.l[k] += orbital[k] + spin[k];
        out.mass += m->body_mass[b];
    }
    for (double &x : out.com)
        x /= out.mass;
    return out;
}
} // namespace
bool verifyControlReplay(const QJsonObject &report, QJsonObject &metrics, QString &error) {
    try {
        need(mj_version() == 337, "Replay requires MuJoCo 3.3.7.");
        auto record = report["controlReplay"].toObject();
        auto xml = record["modelXml"].toString().toUtf8();
        auto hash = [](const QByteArray &x) {
            return QString::fromLatin1(QCryptographicHash::hash(x, QCryptographicHash::Sha256).toHex());
        };
        need(hash(xml) == report["modelSha256"].toString(), "Replay MJCF hash mismatch.");
        auto compressed = QByteArray::fromBase64(record["data"].toString().toLatin1());
        need(compressed.size() > 4 && compressed.size() <= 160000000 &&
                 qFromBigEndian<quint32>(compressed.constData()) <= 160000000,
             "Replay exceeds bounded payload size.");
        auto bytes = qUncompress(compressed);
        need(hash(bytes) == record["sha256"].toString(), "Replay data hash mismatch.");
        QDataStream stream(bytes);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
        quint32 magic, nq, nv, nu, steps;
        stream >> magic >> nq >> nv >> nu >> steps;
        need(magic == 0x54524b31 && nq && nv && nu && steps &&
                 quint64(nq) + nv + quint64(steps + 1) * nq + quint64(steps) * nu <= 20000000,
             "Invalid replay dimensions/version.");
        char message[2048]{};
        std::unique_ptr<mjSpec, decltype(&mj_deleteSpec)> spec(
            mj_parseXMLString(xml.constData(), nullptr, message, sizeof(message)), mj_deleteSpec);
        need(bool(spec), message);
        std::unique_ptr<mjModel, decltype(&mj_deleteModel)> m(mj_compile(spec.get(), nullptr),
                                                              mj_deleteModel);
        need(bool(m), mjs_getError(spec.get()));
        need(m->nq == nq && m->nv == nv && m->nu == nu, "Replay dimensions differ from compiled MJCF.");
        need(m->neq == 0 && m->nmocap == 0 && m->nplugin == 0 && m->na == 0,
             "Replay model contains external anchors or unsupported active state.");
        for (int u = 0; u < m->nu; ++u) {
            need(m->actuator_trntype[u] == mjTRN_JOINT && m->actuator_forcelimited[u],
                 "Only bounded internal joint actuators are permitted.");
            int j = m->actuator_trnid[2 * u];
            need(j >= 0 && m->jnt_type[j] != mjJNT_FREE, "A free-root actuator is forbidden.");
        }
        std::unique_ptr<mjData, decltype(&mj_deleteData)> d(mj_makeData(m.get()), mj_deleteData);
        auto read = [&](double *dst, int n) {
            for (int i = 0; i < n; ++i) {
                stream >> dst[i];
                need(std::isfinite(dst[i]), "Nonfinite replay number.");
            }
        };
        read(d->qpos, nq);
        read(d->qvel, nv);
        std::vector<double> positions(quint64(steps + 1) * nq), controls(quint64(steps) * nu), tangent(nv);
        read(positions.data(), int(positions.size()));
        read(controls.data(), int(controls.size()));
        need(stream.status() == QDataStream::Ok && stream.atEnd(), "Truncated or trailing replay data.");
        mj_forward(m.get(), d.get());
        double replay2 = 0, replayPeak = 0, f2 = 0, t2 = 0, peakF = 0, peakT = 0, torqueRatio = 0;
        const double dt = m->opt.timestep;
        for (quint32 i = 0; i < steps; ++i) {
            auto before = momentum(m.get(), d.get());
            mju_copy(d->ctrl, controls.data() + quint64(i) * nu, nu);
            mj_step(m.get(), d.get());
            double force[3], torque[3];
            for (int k = 0; k < 3; ++k)
                force[k] = before.mass * m->opt.gravity[k];
            mju_cross(torque, before.com, force);
            for (int c = 0; c < d->ncon; ++c) {
                const auto &contact = d->contact[c];
                int a = m->geom_bodyid[contact.geom[0]], b = m->geom_bodyid[contact.geom[1]];
                if ((a == 0) == (b == 0))
                    continue; // Internal pairs cancel in whole-system momentum.
                double local[6], f[3]{}, t[3]{}, arm[3];
                mj_contactForce(m.get(), d.get(), c, local);
                const double sign = a == 0 ? 1 : -1;
                for (int k = 0; k < 3; ++k)
                    for (int j = 0; j < 3; ++j) {
                        f[k] += sign * contact.frame[3 * j + k] * local[j];
                        t[k] += sign * contact.frame[3 * j + k] * local[j + 3];
                    }
                mju_cross(arm, contact.pos, f);
                for (int k = 0; k < 3; ++k) {
                    force[k] += f[k];
                    torque[k] += t[k] + arm[k];
                }
            }
            for (int u = 0; u < m->nu; ++u) {
                const double bound = std::max(std::abs(m->actuator_forcerange[2 * u]),
                                              std::abs(m->actuator_forcerange[2 * u + 1]));
                torqueRatio = std::max(torqueRatio, std::abs(d->actuator_force[u]) / std::max(1e-12, bound));
            }
            mj_forward(m.get(), d.get());
            auto after = momentum(m.get(), d.get());
            double forceResidual[3], torqueResidual[3];
            for (int k = 0; k < 3; ++k) {
                forceResidual[k] = (after.p[k] - before.p[k]) / dt - force[k];
                torqueResidual[k] = (after.l[k] - before.l[k]) / dt - torque[k];
            }
            double fr = mju_dot3(forceResidual, forceResidual), tr = mju_dot3(torqueResidual, torqueResidual);
            f2 += fr;
            t2 += tr;
            peakF = std::max(peakF, std::sqrt(fr));
            peakT = std::max(peakT, std::sqrt(tr));
            mj_differentiatePos(m.get(), tangent.data(), 1, d->qpos, positions.data() + quint64(i + 1) * nq);
            for (double e : tangent) {
                replay2 += e * e;
                replayPeak = std::max(replayPeak, std::abs(e));
            }
            need(d->warning[mjWARN_BADQPOS].number == 0 && d->warning[mjWARN_BADQVEL].number == 0 &&
                     d->warning[mjWARN_BADQACC].number == 0,
                 "Invalid state during independent replay.");
        }
        auto norm = report["normalization"].toObject();
        double forceScale = norm["forceN"].toDouble(), torqueScale = norm["torqueNm"].toDouble();
        double forceRms = std::sqrt(f2 / steps), torqueRms = std::sqrt(t2 / steps);
        bool match = replayPeak <= 1e-6;
        bool balance = forceScale > 0 && torqueScale > 0 && forceRms <= .03 * forceScale &&
                       torqueRms <= .03 * torqueScale && peakF <= .1 * forceScale &&
                       peakT <= .1 * torqueScale;
        metrics = {{"engine", mj_versionString()},
                   {"steps", int(steps)},
                   {"substepSeconds", dt},
                   {"trajectoryTangentRms", std::sqrt(replay2 / (double(steps) * nv))},
                   {"trajectoryTangentPeak", replayPeak},
                   {"linearMomentumBalanceRmsN", forceRms},
                   {"angularMomentumBalanceRmsNm", torqueRms},
                   {"linearMomentumBalancePeakN", peakF},
                   {"angularMomentumBalancePeakNm", peakT},
                   {"actuatorLimitRatio", torqueRatio},
                   {"trajectoryMatched", match},
                   {"momentumBalancePassed", balance},
                   {"externalGeneralizedForces", false},
                   {"externalCartesianForces", false},
                   {"rootActuators", 0},
                   {"qposAssignments", 1},
                   {"angularMomentumReference", "world origin; orbital + spin"},
                   {"method", "independent body COM Jacobian velocities, world principal inertia, finite "
                              "impulse balance against gravity + native ground contact wrenches"}};
        need(match, "Saved controls did not reproduce the native trajectory.");
        need(torqueRatio <= 1.01, "Actuator limit exceeded in replay.");
        need(balance, "Independent whole-system impulse balance exceeds the fixed root residual tolerances.");
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
} // namespace trickster
