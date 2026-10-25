#include "edit_service.h"
#include "physics_demos.h"
#include "physics_provenance.h"
#include "physics_proxy.h"
#include "physics_replay.h"
#include "physics_tools.h"
#include "physics_trajectory.h"
#include "physics_validation.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtTest>
#include <algorithm>
#include <cmath>
using namespace trickster;
namespace {
Scene analyticalBody() {
    Scene s;
    s.nodes = {{"Sphere", -1, {{0, 2, 0}, {}, {1, 1, 1}}, true, {}}};
    s.pose = s.restPose();
    s.keys[0] = {s.pose, {}, {}};
    s.last = 30;
    s.bodies = {{0, 2, .1f, {}, {}}};
    s.physics.ground = -100;
    s.physics.externalAnchors = false;
    return s;
}
void writeJson(const QString &path, const QJsonObject &o) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        qFatal("Cannot write diagnostic");
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}
bool writeExportReference(const QString &folder, Scene &out, QString &error) {
    QJsonArray reference;
    for (double fraction : {0., .137, .5, .823, 1.}) {
        float f = float(out.first + fraction * (out.last - out.first));
        out.evaluate(f);
        QJsonObject joints, transforms;
        QJsonArray vertices;
        auto p = out.positions(out.pose);
        auto globals = out.globals(out.pose);
        for (int n = 0; n < int(out.nodes.size()); ++n) {
            joints[out.nodes[n].name] = QJsonArray{p[n].x(), p[n].y(), p[n].z()};
            QJsonArray matrix;
            for (int k = 0; k < 16; ++k)
                matrix.append(globals[n].constData()[k]);
            transforms[out.nodes[n].name] = matrix;
        }
        for (const auto &mesh : out.meshes)
            for (auto v : out.skinned(mesh))
                vertices.append(QJsonArray{v.x(), v.y(), v.z()});
        reference.append(QJsonObject{{"seconds", double(f - out.first) / out.fps},
                                     {"joints", joints},
                                     {"transforms", transforms},
                                     {"vertices", vertices}});
    }
    QFile expected(folder + "/expected.json");
    if (!expected.open(QIODevice::WriteOnly)) {
        error = expected.errorString();
        return false;
    }
    expected.write(QJsonDocument(reference).toJson(QJsonDocument::Compact));
    return true;
}
struct NativeRecord {
    int nq = 0, nv = 0, nu = 0, steps = 0;
    double dt = 0;
    std::vector<double> q, controls, initialVelocity;
    void read(const QJsonObject &report, const mjModel *m) {
        auto record = report["controlReplay"].toObject();
        auto bytes = qUncompress(QByteArray::fromBase64(record["data"].toString().toLatin1()));
        if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) !=
            record["sha256"].toString())
            qFatal("Native payload hash mismatch");
        QDataStream stream(bytes);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
        quint32 magic, a, b, c, d;
        stream >> magic >> a >> b >> c >> d;
        nq = int(a);
        nv = int(b);
        nu = int(c);
        steps = int(d);
        dt = m->opt.timestep;
        if (magic != 0x54524b31 || nq != m->nq || nv != m->nv || nu != m->nu || steps > 100000)
            qFatal("Native payload dimensions");
        std::vector<double> initial(nq);
        initialVelocity.resize(nv);
        for (auto &x : initial)
            stream >> x;
        for (auto &x : initialVelocity)
            stream >> x;
        q.resize(size_t(steps + 1) * nq);
        controls.resize(size_t(steps) * nu);
        for (auto &x : q)
            stream >> x;
        for (auto &x : controls)
            stream >> x;
        if (stream.status() != QDataStream::Ok || !stream.atEnd())
            qFatal("Native payload truncated");
    }
    void sample(const mjModel *m, double t, double *out) const {
        double u = std::clamp(t / dt, 0., double(steps));
        int i = std::min(int(u), steps - 1);
        mju_copy(out, q.data() + size_t(i) * nq, nq);
        std::vector<double> delta(nv);
        mj_differentiatePos(m, delta.data(), 1., out, q.data() + size_t(i + 1) * nq);
        mj_integratePos(m, out, delta.data(), u - i);
    }
};
} // namespace
class ReliabilityTests : public QObject {
    Q_OBJECT
  private slots:
    void analyticalDerivativeAndCoordinates() {
        auto s = analyticalBody();
        mj::Backend b;
        QString e;
        QVERIFY2(b.compile(s, e), qPrintable(e));
        auto m = b.model.get();
        // Exact quadratic translation and constant-axis rotation. No float/editor conversion.
        auto sample = [&](double t, double *q) {
            mju_copy(q, m->qpos0, m->nq);
            q[0] = .2 * t + .5 * .3 * t * t;
            q[1] = 2 + .4 * t - .5 * s.physics.gravity * t * t;
            double axis[3] = {.2672612419124244, .5345224838248488, .8017837257372732};
            mju_axisAngle2Quat(q + 3, axis, .7 * t);
        };
        for (int fps : {30, 60, 120, 240}) {
            auto a = sampledConfigurationInverse(b, sample, 0, 1, fps);
            // External x acceleration is 0.3 m/s2, mass 2 kg. Free rotation sphere is torque-free.
            QVERIFY2(std::abs(a["rootForceRmsN"].toDouble() - .6) < 1e-7,
                     qPrintable(QJsonDocument(a).toJson()));
            QVERIFY(a["rootTorqueRmsNm"].toDouble() < 1e-7);
        }
    }
    void endRestPreservesInitialVelocity() {
        auto source = analyticalBody();
        source.last = 2;
        source.physics.gravity = 0;
        source.nodes.push_back({"Child", 0, {{0, .5f, 0}, {}, {1, 1, 1}}, true, {}});
        source.bodies.push_back({1, 1, .07f, {0, .15f, 0}, {0, .3f, 0}});
        for (int f = 0; f <= 2; f++) {
            source.pose = source.restPose();
            source.pose[0].t.setX(.001f * f);
            source.keys[f] = {source.pose, {}, {}};
        }
        source.evaluate(0);
        DynamicsSettings settings;
        settings.iterations = 1;
        settings.seconds = 1;
        settings.endAtRest = true;
        settings.prepareLanding = false;
        settings.outputFps = 30;
        settings.importantFrames = {{1, 10}};
        Scene result;
        PhysicsReport report;
        QString error;
        QVERIFY2(optimizeDynamics(source, 0, 2, settings, result, report, error), qPrintable(error));
        mj::Backend backend;
        QVERIFY2(backend.compile(source, error), qPrintable(error));
        NativeRecord native;
        native.read(report.json(), backend.model.get());
        QVERIFY(std::abs(native.initialVelocity[0] - .03) < 1e-6);
        QVERIFY(!report.details["prepareLanding"].toBool(true));
    }
    void physicalJointModelSurvivesBakeAndReload() {
        auto source = physicsDemo("squat");
        QString e;
        mj::Backend initial;
        QVERIFY(initial.compile(source, e));
        auto baked = physicsBake(source, nullptr, 60);
        QVERIFY(baked.jointLimits.empty());
        QVERIFY(baked.physics.hasJointModel);
        Scene reopened;
        QVERIFY2(sceneFromJson(sceneToJson(baked), reopened, e), qPrintable(e));
        mj::Backend model;
        QVERIFY2(model.compile(reopened, e), qPrintable(e));
        QCOMPARE(model.model->nq, initial.model->nq);
        QCOMPARE(model.model->nv, initial.model->nv);
        QCOMPARE(model.xml, initial.xml);
        int frame = 0;
        EditService::apply(reopened, frame, "pose_limits", {{"node", 14}, {"enabled", false}});
        QVERIFY(!reopened.physics.modelJointLimits.contains(14));
        auto invalid = sceneToJson(baked);
        auto physics = invalid["physics"].toObject();
        auto joint = physics["jointModel"].toObject();
        auto directed = joint["directed"].toArray();
        auto first = directed[0].toObject();
        first["axis"] = QJsonArray{0, 0, 0};
        directed[0] = first;
        joint["directed"] = directed;
        physics["jointModel"] = joint;
        invalid["physics"] = physics;
        Scene rejected;
        QVERIFY(!sceneFromJson(invalid, rejected, e));
    }
    void representationRateKeepsSimulationRate() {
        auto source = analyticalBody();
        source.physics.substeps = 64;
        source.physicsResult = {{"status", "solved"}};
        QString error;
        mj::Backend original;
        QVERIFY(original.compile(source, error));
        for (int fps : {30, 60, 120, 240}) {
            auto baked = source.resampledCopy(fps);
            mj::Backend b;
            QVERIFY2(b.compile(baked, error), qPrintable(error));
            QCOMPARE(b.model->opt.timestep, original.model->opt.timestep);
            QVERIFY(baked.physicsResult["validationStale"].toBool());
        }
        QVERIFY(!source.physicsResult["validationStale"].toBool());
    }
    void derivativeFrequencyCalibration() {
        auto s = analyticalBody();
        mj::Backend backend;
        QString error;
        QVERIFY(backend.compile(s, error));
        auto m = backend.model.get();
        QJsonArray measurements;
        for (int fps : {30, 60, 120, 240}) {
            constexpr double a = .7, b = .4;
            auto rotation = [&](double t, double *q) {
                mju_copy(q, m->qpos0, m->nq);
                q[1] = 2 - .5 * s.physics.gravity * t * t;
                double rx[4], ry[4], x[3] = {1, 0, 0}, y[3] = {0, 1, 0};
                mju_axisAngle2Quat(rx, x, a * t);
                mju_axisAngle2Quat(ry, y, b * t * t);
                mju_mulQuat(q + 3, rx, ry);
            };
            auto measured = sampledConfigurationInverse(backend, rotation, 0, 1, fps);
            double expected = .008 * 2 * b * std::sqrt(1 + a * a * (2 * fps - 1) / (6. * fps));
            QVERIFY(std::abs(measured["rootTorqueRmsNm"].toDouble() / expected - 1) < .002);
            for (double frequency : {1., 10., 100.}) {
                auto oscillation = [&](double t, double *q) {
                    mju_copy(q, m->qpos0, m->nq);
                    q[0] = .1 * std::sin(2 * M_PI * frequency * t);
                    q[1] = 2 - .5 * s.physics.gravity * t * t;
                };
                auto metric = sampledConfigurationInverse(backend, oscillation, 0, 1, fps);
                double gain = std::pow(std::sin(M_PI * frequency / fps) / (M_PI * frequency / fps), 2);
                double expectedDiscrete2 = 0;
                for (int i = 1; i < fps; i++)
                    expectedDiscrete2 += std::pow(2 * .1 * std::pow(2 * M_PI * frequency, 2) *
                                                      std::sin(2 * M_PI * frequency * i / fps) * gain,
                                                  2);
                double expectedDiscrete = std::sqrt(expectedDiscrete2 / (fps - 1));
                QVERIFY(std::abs(metric["rootForceRmsN"].toDouble() - expectedDiscrete) < 1e-6);
                measurements.append(QJsonObject{{"analysisFps", fps},
                                                {"frequencyHz", frequency},
                                                {"accelerationGain", gain},
                                                {"measured", metric},
                                                {"knownDiscreteForceRmsN", expectedDiscrete},
                                                {"aboveNyquist", frequency > fps * .5}});
            }
        }
        writeJson(
            "artifacts/reliability/derivative-calibration.json",
            {{"methodVersion", "closed-form-derivative-calibration/1"}, {"measurements", measurements}});
    }
    void quietContactControl() {
        auto s = analyticalBody();
        s.physics.ground = 0;
        s.physics.substeps = 64;
        mj::Backend backend;
        QString error;
        QVERIFY(backend.compile(s, error));
        auto m = backend.model.get();
        auto d = backend.data();
        d->qpos[1] = .101;
        for (int i = 0; i < 6000; i++)
            mj_step(m, d.get());
        mj_forward(m, d.get());
        auto settled = std::vector<double>(d->qpos, d->qpos + m->nq);
        auto sample = [&](double, double *q) { mju_copy(q, settled.data(), m->nq); };
        auto inverse = sampledConfigurationInverse(backend, sample, 0, 1, 30);
        auto impulse = motionImpulseBalance(backend, sample, {}, 0, 1, 30, .001);
        qInfo() << "Quiet contact inverse/impulse" << inverse["rootForceRmsN"] << impulse["rootForceRmsN"];
        QVERIFY(inverse["rootForceRmsN"].toDouble() < .03 * 2 * s.physics.gravity);
        QVERIFY(impulse["rootForceRmsN"].toDouble() < .03 * 2 * s.physics.gravity);
        writeJson("artifacts/reliability/quiet-contact.json", {{"inverse", inverse}, {"impulse", impulse}});
    }
    void representedImpulseAnalytical() {
        auto s = analyticalBody();
        mj::Backend b;
        QString e;
        QVERIFY2(b.compile(s, e), qPrintable(e));
        auto m = b.model.get();
        auto ballistic = [&](double t, double *q) {
            mju_copy(q, m->qpos0, m->nq);
            q[0] = .2 * t;
            q[1] = 2 + .4 * t - .5 * s.physics.gravity * t * t;
        };
        for (int fps : {30, 60, 120, 240}) {
            auto a = motionImpulseBalance(b, ballistic, {}, 0, .5, fps, .0001);
            QVERIFY2(a["rootForceRmsN"].toDouble() < .1, qPrintable(QJsonDocument(a).toJson()));
            QVERIFY(a["generalizedRootForceRmsN"].toDouble() < .1);
        }
    }
    void explicitScaleProxy() {
        auto s = analyticalBody();
        s.last = 30;
        s.keys[30] = s.keys[0];
        s.keys[0].pose[0].s = s.keys[30].pose[0].s = {.99999f, 1, .99998f};
        Scene proxy;
        QJsonObject report;
        QString e;
        QVERIFY2(preparePhysicsProxy(s, proxy, report, e), qPrintable(e));
        QVERIFY(report["approximate"].toBool());
        QVERIFY(report["maximumWorldJointErrorM"].toDouble() <= .001);
        QCOMPARE(s.keys[0].pose[0].s.x(), .99999f);
        s.keys[30].pose[0].s.setX(.9f);
        QVERIFY(!preparePhysicsProxy(s, proxy, report, e));
        QVERIFY2(e.contains("Time-varying"),qPrintable(e));
    }
    void staticScaleSkinMorphMapping() {
        auto s = Scene::mannequin();
        s.last = 30;
        s.keys.clear();
        auto &mesh = s.meshes.front();
        if (mesh.node < 0)
            mesh.node = 0;
        MorphTarget target;
        target.name = "Proxy test";
        target.positions.resize(mesh.vertices.size(), QVector3D(0, .01f, 0));
        mesh.targets.push_back(target);
        s.nodes[mesh.node].morphRest = {0};
        for (int f : {0, 30}) {
            s.pose = s.restPose();
            s.pose[0].s = {1.03f, 1.03f, 1.03f};
            s.pose[6].r = QQuaternion::fromAxisAndAngle(1, 0, 0, float(f));
            s.keys[f] = {s.pose, {}, {{mesh.node, {f == 0 ? .2f : .8f}}}};
        }
        Scene proxy;
        QJsonObject report;
        QString e;
        auto original = sceneToJson(s);
        QVERIFY2(preparePhysicsProxy(s, proxy, report, e), qPrintable(e));
        QVERIFY(report["maximumWorldGeometryErrorM"].toDouble() < .001);
        QCOMPARE(sceneToJson(s), original);
        QVERIFY(!proxy.meshes.front().targets.empty());
        Scene reopened;
        QVERIFY2(sceneFromJson(sceneToJson(proxy), reopened, e), qPrintable(e));
        QCOMPARE(reopened.physics.requests["rigProxy"], proxy.physics.requests["rigProxy"]);
        for (float f : {0.f, 4.1f, 15.f, 27.3f, 30.f}) {
            s.evaluate(f);
            reopened.evaluate(f);
            auto a = s.skinned(s.meshes.front()), b = reopened.skinned(reopened.meshes.front());
            for (size_t i = 0; i < a.size(); i++)
                QVERIFY((a[i] - b[i]).length() < .001);
        }
    }
    void optionalHumanMWalk() {
        const QString path = "library/TestCharacter/HumanM_Walk01_Forward.glb";
        if (!QFile::exists(path))
            QSKIP("Private optional HumanM walk not installed");
        Scene source, proxy;
        QJsonObject report;
        QString e;
        QVERIFY2(loadCharacter(path, source, e), qPrintable(e));
        bool ready = preparePhysicsProxy(source, proxy, report, e);
        writeJson("artifacts/reliability/HumanM-proxy.json", report);
        qInfo() << "HumanM proxy" << ready << report << e;
        QVERIFY2(ready, qPrintable(e));
        proxy.profile = proxy.suggestedProfile();
        proxy.profile.configured = true;
        proxy.bodies = humanPhysicsRig(proxy);
        fitFootPads(proxy);
        proxy.last = std::min(proxy.last, 30);
        proxy.physics.substeps = 128;
        proxy.physics.externalAnchors = false;
        DynamicsSettings settings;
        settings.iterations = 8;
        settings.seconds = 20;
        settings.outputFps = 0;
        settings.importantFrames = {{proxy.first, 10}, {proxy.last, 10}};
        Scene out;
        PhysicsReport dynamics;
        bool ok = optimizeDynamics(proxy, proxy.first, proxy.last, settings, out, dynamics, e);
        writeJson("artifacts/reliability/HumanM-dynamics.json",
                  {{"calculated", ok}, {"error", e}, {"report", dynamics.json()}});
        if (ok) {
            QDir().mkpath("artifacts/reliability/HumanM");
            QVERIFY2(saveProject("artifacts/reliability/HumanM/result.trickster", out, e), qPrintable(e));
            QVERIFY2(exportAnimation("artifacts/reliability/HumanM/result.glb", out, e), qPrintable(e));
            QVERIFY2(exportFbx("artifacts/reliability/HumanM/result.fbx", out, e), qPrintable(e));
            QVERIFY2(writeExportReference("artifacts/reliability/HumanM", out, e), qPrintable(e));
        }
        qInfo() << "HumanM dynamics" << ok << dynamics.status << e;
    }
    void falseNativeImprovementDoesNotCertifyBake() {
        auto s = analyticalBody();
        s.physics.gravity = 0;
        for (int f = 0; f <= 30; f++) {
            s.pose = s.restPose();
            s.keys[f] = {s.pose, {}, {}};
        }
        auto corrupted = s;
        corrupted.keys[15].pose[0].t.setX(.2f);
        mj::Backend b;
        QString e;
        QVERIFY2(b.compile(s, e), qPrintable(e));
        auto before = sampledMotionInverse(b, s, 0, 1, 60);
        auto after = sampledMotionInverse(b, corrupted, 0, 1, 60);
        QVERIFY(inverseResidualPassed(before, 2, 0, 1));
        QVERIFY(!inverseResidualPassed(after, 2, 0, 1));
        QVERIFY(after["rootForceRmsN"].toDouble() > 100);
        // A virtually zero native-consistency number must never replace this measured after value.
        auto state = b.data();
        mj_forward(b.model.get(), state.get());
        mj_inverse(b.model.get(), state.get());
        double consistency = 0;
        for (int k = 0; k < 6; k++)
            consistency += state->qfrc_inverse[k] * state->qfrc_inverse[k];
        QVERIFY(consistency < 1e-18);
        QJsonObject native{{"rootForceRmsN", std::sqrt(consistency)}};
        QVERIFY(native["rootForceRmsN"].toDouble() < .7 * after["rootForceRmsN"].toDouble());
        QCOMPARE(before["methodVersion"], after["methodVersion"]);
    }
    void fullBodyImportedExperiments() {
        QJsonArray cases;
        for (int rig = 0; rig < 2; rig++)
            for (QString kind : {"squat", "landing"})
                for (int variant = 0; variant < 3; variant++) {
                    if (qEnvironmentVariableIntValue("TRICKSTER_RELIABILITY_QUICK") == 1 && variant != 1)
                        continue;
                    Scene source = physicsDemo(kind);
                    if (kind == "landing") {
                        auto stand = source.restPose();
                        auto feet = source.positions(stand);
                        for (int f = 6; f <= 30; f++) {
                            source.pose = stand;
                            source.pose[0].t.setY(stand[0].t.y() - .12f * std::sin((f - 6) * M_PI / 24));
                            source.reach(15, feet[15], source.defaultPole(15), 1);
                            source.reach(19, feet[19], source.defaultPole(19), 1);
                            source.keys[f].pose = source.pose;
                        }
                    }
                    source.physics.substeps = kind == "landing" ? 256 : 128;
                    source.name = QString("Full body %1 %2 variant %3").arg(rig).arg(kind).arg(variant);
                    // Ranges are fixed before search: amplitude 0.8/1/1.2, duration 0.8/1/1.2 s.
                    double amplitude = variant == 0 ? .8 : variant == 1 ? 1. : 1.2;
                    auto rest = source.restPose();
                    for (auto it = source.keys.begin(); it != source.keys.end(); ++it) {
                        double u = double(it.key()) / source.last;
                        it->pose[0].t = rest[0].t + (it->pose[0].t - rest[0].t) * float(amplitude);
                        it->pose[2].r = QQuaternion::fromAxisAndAngle(
                            0, 0, 1, float(3 * std::sin(2 * M_PI * u) * std::pow(std::sin(M_PI * u), 2)));
                        it->pose[6].r = QQuaternion::fromAxisAndAngle(
                            0, 0, 1, float(-8 * std::pow(std::sin(M_PI * u), 2)));
                        it->pose[10].r = QQuaternion::fromAxisAndAngle(
                            0, 0, 1, float(6 * std::pow(std::sin(M_PI * u), 2)));
                    }
                    source.fps = variant == 0 ? 40 : variant == 1 ? 30 : 25;
                    source.contacts.clear();
                    source.evaluate(0);
                    if (rig == 1) {
                        // A second procedural skeleton: different proportions as well as local axes/helpers.
                        for (int n : {14, 18, 7, 11}) {
                            float factor = (n == 14 || n == 18) ? 1.08f : .90f;
                            source.nodes[n].rest.t *= factor;
                            for (auto it = source.keys.begin(); it != source.keys.end(); ++it)
                                it->pose[n].t *= factor;
                        }
                        auto feet = source.positions(source.restPose());
                        float lift = .1f - std::min(feet[15].y(), feet[19].y());
                        source.nodes[0].rest.t.setY(source.nodes[0].rest.t.y() + lift);
                        for (auto it = source.keys.begin(); it != source.keys.end(); ++it)
                            it->pose[0].t.setY(it->pose[0].t.y() + lift);
                        auto resizedGlobals = source.globals(source.restPose());
                        for (auto &mesh : source.meshes) {
                            std::vector<QMatrix4x4> mapping;
                            for (size_t j = 0; j < mesh.joints.size(); j++)
                                mapping.push_back(resizedGlobals[mesh.joints[j]] * mesh.inverseBind[j]);
                            for (auto &v : mesh.vertices) {
                                QVector3D point, normal;
                                float sum = 0;
                                for (int j = 0; j < 8; j++)
                                    if (v.weights[j] > 0) {
                                        point += mapping[v.joints[j]].map(v.p) * v.weights[j];
                                        normal += mapping[v.joints[j]].mapVector(v.normal) * v.weights[j];
                                        sum += v.weights[j];
                                    }
                                if (sum > 0) {
                                    v.p = point / sum;
                                    v.normal = normal.normalized();
                                }
                            }
                            for (size_t j = 0; j < mesh.joints.size(); j++)
                                mesh.inverseBind[j] = resizedGlobals[mesh.joints[j]].inverted();
                        }
                        std::vector<QQuaternion> basis(source.nodes.size());
                        for (size_t n = 0; n < source.nodes.size(); n++)
                            basis[n] = QQuaternion::fromAxisAndAngle(1, 1, 0, float((int(n) % 3 - 1) * 25));
                        auto change = [&](Pose &pose) {
                            for (size_t n = 0; n < pose.size(); n++) {
                                auto parent = source.nodes[n].parent;
                                auto p = parent < 0 ? QQuaternion{} : basis[parent];
                                pose[n].t = p.conjugated().rotatedVector(pose[n].t);
                                pose[n].r = (p.conjugated() * pose[n].r * basis[n]).normalized();
                            }
                        };
                        Pose newRest = source.restPose();
                        change(newRest);
                        for (size_t n = 0; n < source.nodes.size(); n++) {
                            source.nodes[n].rest = newRest[n];
                            source.nodes[n].name = "HelperRig_" + source.nodes[n].name;
                        }
                        for (auto it = source.keys.begin(); it != source.keys.end(); ++it)
                            change(it->pose);
                        for (auto &mesh : source.meshes)
                            for (size_t j = 0; j < mesh.joints.size(); j++) {
                                QMatrix4x4 inv;
                                inv.rotate(basis[mesh.joints[j]].conjugated());
                                mesh.inverseBind[j] = inv * mesh.inverseBind[j];
                            }
                        for (auto it = source.jointLimits.begin(); it != source.jointLimits.end(); ++it)
                            it->axis = basis[it.key()].conjugated().rotatedVector(it->axis);
                        for (int child : {6, 10, 13, 17}) {
                            int helper = int(source.nodes.size());
                            auto h = QQuaternion::fromAxisAndAngle(0, 1, 0, 17);
                            source.nodes.push_back({QString("AxisHelper%1").arg(child),
                                                    source.nodes[child].parent,
                                                    {{}, h, {1, 1, 1}},
                                                    true,
                                                    {}});
                            source.nodes[child].parent = helper;
                            auto fix = [&](Pose &pose) {
                                pose[child].t = h.conjugated().rotatedVector(pose[child].t);
                                pose[child].r = (h.conjugated() * pose[child].r).normalized();
                                pose.push_back({{}, h, {1, 1, 1}});
                            };
                            for (auto it = source.keys.begin(); it != source.keys.end(); ++it)
                                fix(it->pose);
                            source.nodes[child].rest.t =
                                h.conjugated().rotatedVector(source.nodes[child].rest.t);
                            source.nodes[child].rest.r =
                                (h.conjugated() * source.nodes[child].rest.r).normalized();
                        }
                    }
                    source.pose = source.restPose();
                    source.evaluate(0);
                    source.bodies = humanPhysicsRig(source);
                    // Foot boxes follow the actual local frame; all 21 major bodies remain separate.
                    fitFootPads(source);
                    auto p = source.positions(source.restPose());
                    for (int foot : {15, 19}) {
                        Contact c;
                        c.node = foot;
                        c.first = kind == "landing" ? 5 : 0;
                        c.last = 30;
                        c.fadeIn = kind == "landing" ? 1 : 0;
                        c.target = p[foot];
                        c.pole = source.defaultPole(foot);
                        c.orient = true;
                        c.orientation = source.globalRotation(foot, source.restPose());
                        source.contacts.push_back(c);
                    }
                    QString id = QString("rig%1-%2-v%3").arg(rig).arg(kind).arg(variant),
                            folder = "artifacts/reliability/fullbody/" + id;
                    QDir().mkpath(folder);
                    QString e;
                    QVERIFY2(exportAnimation(folder + "/source.glb", source, e), qPrintable(e));
                    Scene imported;
                    QVERIFY2(loadCharacter(folder + "/source.glb", imported, e), qPrintable(e));
                    imported.profile = source.profile;
                    imported.bodies = source.bodies;
                    imported.physics = source.physics;
                    imported.contacts = source.contacts;
                    imported.jointLimits = source.jointLimits;
                    imported.rotationLimits = source.rotationLimits;
                    QVERIFY(imported.bodies.size() >= 21);
                    DynamicsSettings settings;
                    settings.iterations = 12;
                    settings.seconds = 15;
                    settings.outputFps = source.fps == 40 ? 80 : source.fps == 25 ? 50 : 60;
                    settings.importantFrames = {{0, 10}, {15, 5}, {30, 10}};
                    settings.endAtRest = true;
                    Scene out;
                    PhysicsReport report;
                    QVERIFY2(saveProject(folder + "/source.trickster", imported, e), qPrintable(e));
                    bool ok = optimizeDynamics(imported, 0, 30, settings, out, report, e);
                    QJsonObject record{{"id", id},
                                       {"authoringVersion", 3},
                                       {"amplitude", amplitude},
                                       {"durationSeconds", 30. / source.fps},
                                       {"physicalBodies", int(imported.bodies.size())},
                                       {"nodes", int(imported.nodes.size())},
                                       {"calculated", ok},
                                       {"error", e},
                                       {"report", report.json()}};
                    qInfo() << id << "calculated" << ok << report.status << "native"
                            << report.details["nativeForwardVerified"] << "goal m" << report.maxTargetPosition
                            << e;
                    if (ok) {
                        QVERIFY2(saveProject(folder + "/result.trickster", out, e), qPrintable(e));
                        Scene reopened;
                        QVERIFY2(loadProject(folder + "/result.trickster", reopened, e), qPrintable(e));
                        QCOMPARE(reopened.physicsResult, out.physicsResult);
                        QVERIFY2(exportAnimation(folder + "/result.glb", reopened, e), qPrintable(e));
                        QVERIFY2(exportFbx(folder + "/result.fbx", reopened, e), qPrintable(e));
                        QVERIFY2(writeExportReference(folder, reopened, e), qPrintable(e));
                    }
                    cases.append(record);
                    writeJson("artifacts/reliability/fullbody/results.json",
                              {{"cases", cases}, {"environment", physicsEnvironment()}});
                }
    }
    void fullBodyJumpExperiment() {
        auto s = Scene::mannequin();
        s.last = 45;
        s.keys.clear();
        s.profile = s.suggestedProfile();
        s.profile.configured = true;
        s.bodies = humanPhysicsRig(s);
        s.physics.substeps = 256;
        s.physics.contactTime = .004f;
        s.physics.stiffness = 200;
        s.physics.damping = 20;
        s.physics.maxTorque = 200;
        s.physics.externalAnchors = false;
        fitFootPads(s);
        auto stand = s.restPose();
        auto feet = s.positions(stand);
        const double flightDuration = 13. / 30, launch = .5 * s.physics.gravity * flightDuration;
        for (int f = 0; f <= 45; f++) {
            s.pose = stand;
            if (f <= 10) {
                double u = f / 10., u2 = u * u, u3 = u2 * u;
                s.pose[0].t.setY(stand[0].t.y() +
                                 float((2 * u3 - 3 * u2 + 1) * (-.08) + (u3 - u2) * (10. / 30) * launch));
                s.reach(15, feet[15], s.defaultPole(15), 1);
                s.reach(19, feet[19], s.defaultPole(19), 1);
            } else if (f < 23) {
                double t = (f - 10) / 30., u = t / flightDuration;
                s.pose[0].t.setY(stand[0].t.y() + float(launch * t - .5 * s.physics.gravity * t * t));
                for (int hip : {13, 17})
                    s.pose[hip].r =
                        QQuaternion::fromAxisAndAngle(1, 0, 0, float(-20 * std::pow(std::sin(M_PI * u), 2)));
                for (int knee : {14, 18})
                    s.pose[knee].r =
                        QQuaternion::fromAxisAndAngle(1, 0, 0, float(40 * std::pow(std::sin(M_PI * u), 2)));
            } else {
                s.pose[0].t.setY(stand[0].t.y() - .12f * std::sin((f - 23) * M_PI / 22));
                s.reach(15, feet[15], s.defaultPole(15), 1);
                s.reach(19, feet[19], s.defaultPole(19), 1);
            }
            double envelope = std::pow(std::sin(f * M_PI / 45), 2);
            s.pose[6].r = QQuaternion::fromAxisAndAngle(0, 0, 1, float(-12 * envelope));
            s.pose[10].r = QQuaternion::fromAxisAndAngle(0, 0, 1, float(10 * envelope));
            s.keys[f] = {s.pose, {}, {}};
        }
        for (int foot : {15, 19})
            for (auto interval : {std::pair<int, int>{0, 10}, {23, 45}}) {
                Contact c;
                c.node = foot;
                c.first = interval.first;
                c.last = interval.second;
                c.fadeIn = interval.first ? 1 : 0;
                c.target = feet[foot];
                c.pole = s.defaultPole(foot);
                c.orient = true;
                s.contacts.push_back(c);
            }
        s.evaluate(0);
        QString e;
        QString folder = "artifacts/reliability/fullbody/jump";
        QDir().mkpath(folder);
        QVERIFY2(saveProject(folder + "/source.trickster", s, e), qPrintable(e));
        DynamicsSettings settings;
        settings.iterations = 24;
        settings.seconds = 30;
        settings.outputFps = 60;
        settings.endAtRest = true;
        settings.importantFrames = {{0, 10}, {10, 5}, {17, 5}, {30, 5}, {45, 10}};
        Scene out;
        PhysicsReport report;
        bool ok = optimizeDynamics(s, 0, 45, settings, out, report, e);
        writeJson(folder + "/report.json", {{"calculated", ok}, {"error", e}, {"report", report.json()}});
        qInfo() << "Full jump" << ok << report.summary() << e;
        if (ok) {
            QVERIFY(saveProject(folder + "/result.trickster", out, e));
            QVERIFY(exportAnimation(folder + "/result.glb", out, e));
            QVERIFY(exportFbx(folder + "/result.fbx", out, e));
            QVERIFY(writeExportReference(folder, out, e));
        }
    }
    void independentImportedDynamics() {
        QJsonObject cases;
        const bool refined = qEnvironmentVariableIntValue("TRICKSTER_RELIABILITY_EXTERNAL_REFINED") == 2;
        const QString captureRoot =
            refined ? "artifacts/reliability/independent-refined/" : "artifacts/reliability/independent/";
        int tested = 0;
        for (QString kind : {"squat", "landing"}) {
            QString dir = captureRoot + kind;
            QFile f(dir + "/trajectory.json");
            if (!f.exists())
                continue;
            QVERIFY(f.open(QIODevice::ReadOnly));
            auto data = QJsonDocument::fromJson(f.readAll()).object();
            Scene source, result;
            QString e;
            QVERIFY2(loadProject(refined ? "artifacts/reliability/refined/" + kind + "-2/source.trickster"
                                         : "examples/physics/" + kind + "-source.trickster",
                                 source, e),
                     qPrintable(e));
            QVERIFY2(loadProject(refined ? "artifacts/reliability/refined/" + kind + "-2/result.trickster"
                                         : "artifacts/reliability/" + kind + "-adaptive/result.trickster",
                                 result, e),
                     qPrintable(e));
            mj::Backend b;
            QVERIFY2(b.compile(source, e), qPrintable(e));
            auto m = b.model.get();
            NativeRecord native;
            native.read(result.physicsResult, m);
            auto control = [&](double t, double *u) {
                int i = std::clamp(int(t / native.dt), 0, native.steps - 1);
                mju_copy(u, native.controls.data() + size_t(i) * native.nu, native.nu);
            };
            QJsonObject formats;
            for (QString ext : {"glb", "fbx"}) {
                auto frames = data["formats"].toObject()[ext].toArray();
                QVERIFY(frames.size() > 2);
                NativeRecord imported;
                imported.nq = m->nq;
                imported.nv = m->nv;
                imported.steps = frames.size() - 1;
                imported.dt = frames[1].toObject()["seconds"].toDouble();
                imported.q.resize(size_t(frames.size()) * m->nq);
                for (int i = 0; i < frames.size(); i++) {
                    auto globals = frames[i].toObject()["transforms"].toObject();
                    auto positions = source.positions(source.restPose());
                    std::vector<QQuaternion> rotations(source.nodes.size());
                    for (size_t n = 0; n < source.nodes.size(); n++) {
                        auto a = globals[source.nodes[n].name].toArray();
                        if (a.isEmpty()) {
                            rotations[n] = source.globalRotation(int(n), source.restPose());
                            continue;
                        }
                        QMatrix4x4 matrix;
                        for (int k = 0; k < 16; k++)
                            matrix.data()[k] = float(a[k].toDouble());
                        positions[n] = matrix.column(3).toVector3D();
                        QMatrix3x3 rot;
                        for (int r = 0; r < 3; r++)
                            for (int c = 0; c < 3; c++)
                                rot(r, c) = matrix(r, c);
                        rotations[n] = QQuaternion::fromRotationMatrix(rot).normalized();
                    }
                    QVERIFY2(b.worldPose(positions, rotations, imported.q.data() + size_t(i) * m->nq, e),
                             qPrintable(e));
                }
                auto sample = [&](double t, double *q) { imported.sample(m, t, q); };
                auto metrics = motionImpulseBalance(b, sample, control, 0, 1, 30, .001);
                metrics["consumer"] = data["consumer"];
                metrics["captureMethod"] = data["captureMethod"];
                metrics["importBakeFps"] = data["bakeFps"];
                metrics["trajectorySampleFps"] = data["sampleFps"];
                formats[ext] = metrics;
                qInfo() << kind << ext << "external impulse" << metrics["rootForceRmsN"]
                        << metrics["rootForcePeakN"];
            }
            cases[kind] = formats;
            ++tested;
        }
        if (!tested)
            QSKIP("Run independent Godot trajectory capture first");
        writeJson(refined ? "artifacts/reliability/external-refined-dynamics.json"
                          : "artifacts/reliability/external-dynamics.json",
                  {{"cases", cases}});
    }
    void simulationStepConvergence() {
        QJsonObject cases;
        for (QString kind : {"squat", "landing"}) {
            Scene source, historical;
            QString e;
            QVERIFY2(loadProject("examples/physics/" + kind + "-source.trickster", source, e), qPrintable(e));
            QVERIFY2(loadProject("examples/physics/" + kind + "-result.trickster", historical, e),
                     qPrintable(e));
            mj::Backend b;
            QVERIFY2(b.compile(source, e), qPrintable(e));
            auto m = b.model.get();
            NativeRecord original;
            original.read(historical.physicsResult, m);
            QJsonArray refinements;
            for (int factor : {1, 2, 4}) {
                m->opt.timestep = original.dt / factor;
                auto d = b.data();
                mju_copy(d->qpos, original.q.data(), m->nq);
                mju_copy(d->qvel, original.initialVelocity.data(), m->nv);
                mj_forward(m, d.get());
                NativeRecord refined = original;
                refined.dt = m->opt.timestep;
                refined.steps = original.steps * factor;
                refined.q.resize(size_t(refined.steps + 1) * m->nq);
                refined.controls.resize(size_t(refined.steps) * m->nu);
                mju_copy(refined.q.data(), d->qpos, m->nq);
                std::vector<std::vector<double>> positions(size_t(refined.steps + 1),
                                                           std::vector<double>(m->nq));
                mju_copy(positions[0].data(), d->qpos, m->nq);
                for (int i = 0; i < refined.steps; i++) {
                    mju_copy(d->ctrl, original.controls.data() + size_t(i / factor) * m->nu, m->nu);
                    mju_copy(refined.controls.data() + size_t(i) * m->nu, d->ctrl, m->nu);
                    mj_step(m, d.get());
                    mj_forward(m, d.get());
                    mju_copy(refined.q.data() + size_t(i + 1) * m->nq, d->qpos, m->nq);
                    mju_copy(positions[i + 1].data(), d->qpos, m->nq);
                }
                auto controls = [&](double t, double *u) {
                    int i = std::clamp(int(t / refined.dt), 0, refined.steps - 1);
                    mju_copy(u, refined.controls.data() + size_t(i) * m->nu, m->nu);
                };
                auto raw = [&](double t, double *q) { refined.sample(m, t, q); };
                auto nativeBalance = motionImpulseBalance(b, raw, controls, 0, 1, 30, .001);
                Scene baked = physicsBake(source, nullptr, 60);
                auto representation = bakePhysicsTrajectory(source, b, positions, 0, 30, baked, true, nullptr,
                                                            refined.initialVelocity);
                auto sample = [&](double t, double *q) {
                    baked.evaluate(float(t * baked.fps));
                    if (!b.pose(baked, q, e))
                        qFatal("Refined pose failed");
                };
                auto balance = motionImpulseBalance(b, sample, controls, 0, 1, 30, .001);
                Scene refinedSource = source;
                refinedSource.physics.substeps *= factor;
                baked.physics.substeps = refinedSource.physics.substeps * source.fps / baked.fps;
                PhysicsReport snapshot;
                snapshot.status = "approximate";
                snapshot.stopReason =
                    "Fixed historical controls at explicitly refined numerical timestep; no new optimization";
                snapshot.bodies = int(b.bodies.size());
                snapshot.dofs = m->nv;
                snapshot.totalMass = physicalMass(source);
                snapshot.details["trajectoryRepresentation"] = representation;
                snapshot.details["representedImpulse"] = balance;
                QString xml = b.xml;
                xml.replace(QRegularExpression("timestep=\"[^\"]*\""),
                            "timestep=\"" + QString::number(m->opt.timestep, 'g', 17) + "\"");
                recordPhysicsProvenance(refinedSource, 0, 30, snapshot, xml);
                std::vector<std::vector<double>> commandRows(size_t(refined.steps),
                                                             std::vector<double>(m->nu));
                for (int i = 0; i < refined.steps; i++)
                    mju_copy(commandRows[i].data(), refined.controls.data() + size_t(i) * m->nu, m->nu);
                recordControlReplay(snapshot, xml, positions.front(), original.initialVelocity, positions,
                                    commandRows);
                QJsonObject replay;
                QString replayError;
                bool reproduced = verifyControlReplay(snapshot.json(), replay, replayError);
                snapshot.details["independentReplay"] = replay;
                snapshot.details["nativeReplayPassed"] = reproduced;
                snapshot.details["replayReason"] = replayError;
                baked.physicsResult = snapshot.json();
                QString folder = QString("artifacts/reliability/refined/%1-%2").arg(kind).arg(factor);
                QDir().mkpath(folder);
                QVERIFY2(saveProject(folder + "/source.trickster", refinedSource, e), qPrintable(e));
                QVERIFY2(saveProject(folder + "/result.trickster", baked, e), qPrintable(e));
                QVERIFY2(exportAnimation(folder + "/result.glb", baked, e), qPrintable(e));
                QVERIFY2(exportFbx(folder + "/result.fbx", baked, e), qPrintable(e));
                QVERIFY2(writeExportReference(folder, baked, e), qPrintable(e));
                writeJson(folder + "/report.json", snapshot.json());
                qInfo() << kind << "refine" << factor << "native" << nativeBalance["rootForceRmsN"]
                        << nativeBalance["rootForcePeakN"] << "adaptive" << balance["rootForceRmsN"]
                        << balance["rootForcePeakN"] << "generalized" << balance["generalizedRootForceRmsN"]
                        << balance["generalizedRootForcePeakN"];
                refinements.append(QJsonObject{{"factor", factor},
                                               {"substepSeconds", m->opt.timestep},
                                               {"native", nativeBalance},
                                               {"adaptive60", balance},
                                               {"representation", representation}});
            }
            cases[kind] = refinements;
        }
        writeJson("artifacts/reliability/step-convergence.json",
                  {{"methodVersion", "fixed-control-step-refinement/1"},
                   {"scope", "Same held controls and initial q/v; only numerical integration step changes. "
                             "Not a new optimized result."},
                   {"cases", cases}});
    }
    void historicalNativeBakeChain() {
        QJsonObject cases;
        for (auto kind : {"squat", "landing"}) {
            QString e;
            Scene source, historical;
            QVERIFY2(loadProject(QString("examples/physics/%1-source.trickster").arg(kind), source, e),
                     qPrintable(e));
            QVERIFY2(loadProject(QString("examples/physics/%1-result.trickster").arg(kind), historical, e),
                     qPrintable(e));
            mj::Backend b;
            QVERIFY2(b.compile(source, e), qPrintable(e));
            auto m = b.model.get();
            NativeRecord native;
            native.read(historical.physicsResult, m);
            auto raw = [&](double t, double *q) { native.sample(m, t, q); };
            QJsonArray experiments;
            double roundTripPeak = 0;
            auto d = b.data();
            std::vector<double> q(m->nq), delta(m->nv);
            Scene reconstructed = physicsBake(source, nullptr, 60);
            for (int i = 0; i <= native.steps; i += std::max(1, native.steps / 240)) {
                mju_copy(d->qpos, native.q.data() + size_t(i) * m->nq, m->nq);
                mj_forward(m, d.get());
                b.reconstruct(reconstructed, d.get());
                QVERIFY2(b.pose(reconstructed, q.data(), e), qPrintable(e));
                mj_differentiatePos(m, delta.data(), 1., d->qpos, q.data());
                for (double x : delta)
                    roundTripPeak = std::max(roundTripPeak, std::abs(x));
            }
            QJsonArray nativeFine;
            for (int rate : {480, 960, 1920, 3840})
                nativeFine.append(sampledConfigurationInverse(b, raw, 0, 1, rate));
            auto controls = [&](double t, double *u) {
                int i = std::min(native.steps - 1, std::max(0, int(t / native.dt)));
                mju_copy(u, native.controls.data() + size_t(i) * native.nu, native.nu);
            };
            auto nativeImpulse = motionImpulseBalance(b, raw, controls, 0, 1, 30, .001);
            qInfo() << kind << "native represented impulse" << nativeImpulse["rootForceRmsN"]
                    << nativeImpulse["rootForcePeakN"];
            for (int representation : {30, 60, 120, 240}) {
                Scene baked = physicsBake(source, nullptr, representation);
                for (int f = 0; f <= representation; f++) {
                    baked.evaluate(float(f));
                    native.sample(m, double(f) / representation, d->qpos);
                    mj_forward(m, d.get());
                    b.reconstruct(baked, d.get());
                    baked.keys[f] = {baked.pose, {}, baked.morphs};
                }
                Scene reopened;
                QVERIFY2(sceneFromJson(sceneToJson(baked), reopened, e), qPrintable(e));
                auto bakedSampler = [&](double t, double *q) {
                    baked.evaluate(float(t * baked.fps));
                    if (!b.pose(baked, q, e))
                        qFatal("Baked pose failed");
                };
                auto impulse = motionImpulseBalance(b, bakedSampler, controls, 0, 1, 30, .001);
                qInfo() << kind << representation << "baked impulse" << impulse["rootForceRmsN"]
                        << impulse["rootForcePeakN"];
                for (int analysis : {30, 60, 120, 240}) {
                    auto rawStats = sampledConfigurationInverse(b, raw, 0, 1, analysis, nullptr, true);
                    auto bakedStats = sampledMotionInverse(b, baked, 0, 1, analysis);
                    auto savedStats = sampledMotionInverse(b, reopened, 0, 1, analysis);
                    experiments.append(QJsonObject{{"representationFps", representation},
                                                   {"analysisFps", analysis},
                                                   {"representedImpulse", impulse},
                                                   {"nativePositions", rawStats},
                                                   {"baked", bakedStats},
                                                   {"reopened", savedStats}});
                    QCOMPARE(bakedStats["rootForceRmsN"], savedStats["rootForceRmsN"]);
                }
            }
            std::vector<std::vector<double>> nativeRows(size_t(native.steps + 1),
                                                        std::vector<double>(native.nq));
            for (int i = 0; i <= native.steps; i++)
                mju_copy(nativeRows[i].data(), native.q.data() + size_t(i) * native.nq, native.nq);
            QJsonArray hermiteResults;
            for (bool adaptive : {false, true}) {
                Scene cubic = physicsBake(source, nullptr, 60);
                auto info = bakePhysicsTrajectory(source, b, nativeRows, 0, 30, cubic, adaptive, nullptr,
                                                  native.initialVelocity);
                auto cubicSample = [&](double t, double *q) {
                    cubic.evaluate(float(t * cubic.fps));
                    if (!b.pose(cubic, q, e))
                        qFatal("Cubic pose failed");
                };
                auto balance = motionImpulseBalance(b, cubicSample, controls, 0, 1, 30, .001);
                qInfo() << kind << "Hermite adaptive" << adaptive << info["physicalKnots"] << "impulse"
                        << balance["rootForceRmsN"] << balance["rootForcePeakN"];
                hermiteResults.append(QJsonObject{{"representation", info}, {"impulse", balance}});
                QString folder =
                    QString("artifacts/reliability/%1-%2").arg(kind).arg(adaptive ? "adaptive" : "cubic60");
                QDir().mkpath(folder);
                cubic.physicsResult = historical.physicsResult;
                cubic.physicsResult["trajectoryRepresentation"] = info;
                cubic.physicsResult["validationStale"] = true;
                QVERIFY2(saveProject(folder + "/result.trickster", cubic, e), qPrintable(e));
                QVERIFY2(exportAnimation(folder + "/result.glb", cubic, e), qPrintable(e));
                QVERIFY2(exportFbx(folder + "/result.fbx", cubic, e), qPrintable(e));
                QVERIFY2(writeExportReference(folder, cubic, e), qPrintable(e));
            }
            qInfo() << kind << "round trip tangent peak" << roundTripPeak;
            cases[kind] = QJsonObject{{"roundTripTangentPeak", roundTripPeak},
                                      {"nativeStepSeconds", native.dt},
                                      {"inputSha256", historical.physicsResult["inputSha256"]},
                                      {"modelSha256", historical.physicsResult["modelSha256"]},
                                      {"experiments", experiments},
                                      {"nativeFine", nativeFine},
                                      {"nativeImpulse", nativeImpulse},
                                      {"hermite", hermiteResults}};
            QVERIFY(roundTripPeak < .001);
        }
        writeJson("artifacts/reliability/baseline-chain.json",
                  QJsonObject{{"methodVersion", "native-bake-chain/1"},
                              {"baseline", "d7ecb38"},
                              {"environment", physicsEnvironment()},
                              {"cases", cases}});
    }
};
QTEST_GUILESS_MAIN(ReliabilityTests)
#include "physics_reliability_tests.moc"
