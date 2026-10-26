#include "io/project/project_io.h"
#include "io/catalog/demo_scenes.h"
#include "core/scene/edit_service.h"
#include "physics/mujoco_backend.h"
#include "physics/physics_demos.h"
#include "physics/physics_tools.h"
#include "physics/physics_validation.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace trickster;
namespace {
Scene body() {
    Scene s;
    s.name = "Analytical sphere";
    s.last = 60;
    s.nodes = {{"Body", -1, {{0, 10, 0}, {}, {1, 1, 1}}, true, {}}};
    s.pose = s.restPose();
    s.bodies = {{0, 2, .1f, {}, {}}};
    s.keys[0] = {s.pose, {}, {}};
    return s;
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
} // namespace
class MuJoCoTests : public QObject {
    Q_OBJECT
  private slots:
    void nativeModelAndMass() {
        auto s = body();
        QString e, xml;
        QVERIFY2(physicsModelXml(s, xml, e), qPrintable(e));
        QVERIFY(xml.contains("freejoint"));
        QVERIFY(!xml.contains("<motor"));
        QCOMPARE(physicsBackendVersion(), QString("MuJoCo 3.3.7"));
        PhysicsMeasurement a;
        QVERIFY2(measurePhysics(s, 0, a, e), qPrintable(e));
        QCOMPARE(a.mass, 2.);
        QVERIFY((a.com - QVector3D(0, 10, 0)).length() < 1e-6);
        for (int i = 0; i < 3; ++i)
            QVERIFY(std::abs(a.inertia(i, i) - .008) < 1e-6);
    }
    void orientedSupportPads() {
        auto s = Scene::mannequin();
        s.bodies = humanPhysicsRig(s);
        fitFootPads(s);
        QString error;
        QVERIFY2(s.validate(&error), qPrintable(error));
        auto p = s.positions(s.pose);
        auto target = groundContactTarget(s, 15, p[15]);
        QVERIFY(std::abs(target.y() - p[15].y()) < 1e-5);
        for (const auto &body : s.bodies)
            if (body.node == 15) {
                QVERIFY(body.footHalfSize.lengthSquared() > 0);
                auto up = (s.globalRotation(15, s.pose) * body.shapeRotation).rotatedVector({0, 1, 0});
                QVERIFY((up - QVector3D(0, 1, 0)).length() < 1e-5);
            }
        auto json = sceneToJson(s);
        Scene loaded;
        QVERIFY(sceneFromJson(json, loaded, error));
        QCOMPARE(loaded.bodies.size(), s.bodies.size());
        QString xml;
        QVERIFY2(physicsModelXml(loaded, xml, error), qPrintable(error));
        QVERIFY(xml.contains("type=\"box\""));
    }
    void humanMassPresetAndImportedCharacter() {
        auto s = Scene::mannequin();
        auto human = humanPhysicsRig(s, 70);
        double sum = 0, head = 0;
        for (auto b : human) {
            sum += b.mass;
            if (b.node == 4)
                head = b.mass;
            QVERIFY(b.estimated);
        }
        QVERIFY(std::abs(sum - 70) < 1e-4);
        QVERIFY(head > 4 && head < 7);
        auto path = qEnvironmentVariable("TRICKSTER_TEST_CHARACTER");
        if (path.isEmpty())
            path = "library/TestCharacter/HumanM_Walk01_Forward.glb";
        if (!QFile::exists(path))
            QSKIP("Optional user character is unavailable; synthetic checks above still run.");
        Scene imported;
        QString error;
        QVERIFY2(loadCharacter(path, imported, error), qPrintable(error));
        imported.bodies = fittedPhysicsRig(imported);
        imported.physics.externalAnchors = false;
        imported.physics.ground = -100;
        imported.physics.gravity = 0;
        PhysicsMeasurement m;
        QDir().mkpath("artifacts/physics-user");
        if (!measurePhysics(imported, imported.first, m, error)) {
            // The converted GLB carries constant, slightly nonuniform animated scale channels.
            // Keep its original animation/export intact and diagnose it; test the native FBX separately.
            QFile diagnostic("artifacts/physics-user/converted-glb-diagnostic.txt");
            QVERIFY(diagnostic.open(QIODevice::WriteOnly));
            diagnostic.write(error.toUtf8());
            diagnostic.close();
            QVERIFY2(error.contains("scale") || error.contains("offset"), qPrintable(error));
            QVERIFY(saveProject("artifacts/physics-user/converted-preserved.jst", imported, error));
            QVERIFY(exportGltf("artifacts/physics-user/converted-preserved.glb", imported, error));
            const QString native = qEnvironmentVariable("TRICKSTER_TEST_FBX");
            QVERIFY2(QFile::exists(native),
                     "Converted fixture is diagnosed; native user FBX fixture is unavailable.");
            QVERIFY2(loadFbx(native, imported, error), qPrintable(error));
            imported.bodies = fittedPhysicsRig(imported);
            imported.physics.externalAnchors = false;
            imported.physics.ground = -100;
            imported.physics.gravity = 0;
            QVERIFY2(measurePhysics(imported, imported.first, m, error), qPrintable(error));
        }
        Scene result;
        PhysicsReport r;
        QVERIFY2(simulatePhysics(imported, imported.first, std::min(imported.first + 5, imported.last),
                                 result, r, error),
                 qPrintable(error));
        QDir().mkpath("artifacts/physics-user");
        QVERIFY(saveProject("artifacts/physics-user/HumanM.jst", result, error));
        QVERIFY(exportGltf("artifacts/physics-user/HumanM.glb", result, error));
        QVERIFY(exportFbx("artifacts/physics-user/HumanM.fbx", result, error));
        QVERIFY(writeExportReference("artifacts/physics-user", result, error));
        QFile f("artifacts/physics-user/report.json");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(r.json()).toJson());
        qInfo() << "Imported native fixture nodes/bodies/DOF" << imported.nodes.size() << r.bodies << r.dofs;
    }
    void externalAnchorWrenchSI() {
        Scene s;
        s.nodes = {{"Root helper", -1, {{0, 8, 0}, {}, {1, 1, 1}}, true, {}},
                   {"Middle helper", 0, {{0, 1, 0}, {}, {1, 1, 1}}, true, {}},
                   {"Held body", 1, {{0, 1, 0}, {}, {1, 1, 1}}, true, {}}};
        s.pose = s.restPose();
        s.keys[0] = {s.pose, {}, {}};
        s.first = 0;
        s.last = 6;
        s.physics.gravity = 9.81;
        s.physics.ground = -100;
        s.physics.externalAnchors = true;
        s.bodies = {{2, 2, .1f, {.1f, 0, 0}, {}}};
        Contact hold{2, 0, 6, {0, 10, 0}, {0, 0, 1}};
        hold.orient = true;
        s.contacts = {hold};
        Scene out;
        PhysicsReport r;
        QString error;
        QVERIFY2(s.validate(&error), qPrintable(error));
        QVERIFY2(simulatePhysics(s, 0, 6, out, r, error), qPrintable(error));
        const double expectedForce = 2 * 9.81, expectedMoment = expectedForce * .1;
        auto samples = r.details["externalAnchorWrenches"].toArray();
        QVERIFY(!samples.empty());
        auto steady = samples.last().toObject();
        auto norm = [](QJsonArray values) {
            double sum = 0;
            for (auto x : values)
                sum += x.toDouble() * x.toDouble();
            return std::sqrt(sum);
        };
        const double force = norm(steady["forceWorldN"].toArray()),
                     moment = norm(steady["momentAboutRootNm"].toArray());
        // The initial compliant constraint can have a transient; test the settled static wrench.
        QVERIFY2(std::abs(force - expectedForce) < .05 * expectedForce, qPrintable(QString::number(force)));
        QVERIFY2(std::abs(moment - expectedMoment) < .05 * expectedMoment,
                 qPrintable(QString::number(moment)));
        QVERIFY(r.maxAnchorForce >= force - 1e-5);
        QVERIFY(r.details["externalAnchorMomentNm"].toDouble() >= moment - 1e-5);
        qInfo() << "Held 2-kg body: steady force N / moment Nm" << force << moment;
    }
    void passiveFullBodyLegacyContacts() {
        auto s = mvpDemo("jump");
        Scene out;
        PhysicsReport r;
        QString error;
        QVERIFY2(simulatePhysics(s, s.first, s.last, out, r, error), qPrintable(error));
        QCOMPARE(out.keys.size(), s.last - s.first + 1);
        QVERIFY(r.maxAnchorForce > 0);
        QVERIFY(out.name.contains("dynamics"));
    }
    void compoundIndependentInertia() {
        auto s = body();
        s.nodes.push_back({"Second", -1, {{4, 10, 0}, {}, {1, 1, 1}}, true, {}});
        s.pose = s.restPose();
        s.keys[0] = {s.pose, {}, {}};
        s.bodies.push_back({1, 6, .1f, {}, {}});
        PhysicsMeasurement a;
        QString e;
        QVERIFY2(measurePhysics(s, 0, a, e), qPrintable(e));
        QVERIFY((a.com - QVector3D(3, 10, 0)).length() < 1e-6);
        QVERIFY(std::abs(a.inertia(0, 0) - .032) < 1e-6);
        QVERIFY(std::abs(a.inertia(1, 1) - 24.032) < .0001);
        QVERIFY(std::abs(a.inertia(2, 2) - 24.032) < .0001);
    }
    void independentMomentumAndPendulum() {
        auto s = body();
        for (int f = 0; f <= 60; ++f) {
            s.pose = s.restPose();
            s.pose[0].t.setX(3.f * f / s.fps);
            s.pose[0].r = QQuaternion::fromAxisAndAngle(0, 0, 1, float(2 * f * 180 / (s.fps * M_PI)));
            s.keys[f] = {s.pose, {}, {}};
        }
        QString error;
        PhysicsMeasurement measurement;
        QVERIFY2(measurePhysics(s, 15, measurement, error), qPrintable(error));
        QVERIFY((measurement.linearMomentum - QVector3D(6, 0, 0)).length() < 1e-4);
        QVERIFY((measurement.angularMomentum - QVector3D(0, 0, .016f)).length() < 1e-5);
        auto pendulum = body();
        pendulum.nodes[0].rest.t = {0, 2, 0};
        pendulum.nodes[0].rest.r = QQuaternion::fromAxisAndAngle(0, 0, 1, float(.1 * 180 / M_PI));
        pendulum.pose = pendulum.restPose();
        pendulum.bodies[0].center = {0, -.5f, 0};
        pendulum.bodies[0].inertia = {.01f, .01f, .01f};
        pendulum.pins[0] = {{0, 2, 0}, {0, 2, 1}, 1};
        pendulum.keys[0] = {pendulum.pose, pendulum.pins, {}};
        mj::Backend b;
        QVERIFY2(b.compile(pendulum, error, true), qPrintable(error));
        auto data = b.data();
        QVERIFY(b.pose(pendulum, data->qpos, error));
        b.updateAnchors(pendulum, 0, data.get());
        mj_forward(b.model.get(), data.get());
        double expected = -2 * 9.81 * .5 * std::sin(.1) / (.01 + 2 * .25);
        qInfo() << "Pendulum angular acceleration" << data->qacc[5] << "analytical" << expected;
        QVERIFY(std::abs(data->qacc[5] - expected) < .01 * std::abs(expected));
    }
    void quaternionRoundTripAndVelocity() {
        auto s = body();
        s.nodes.push_back(
            {"Child", 0, {{0, 1, 0}, QQuaternion::fromEulerAngles(20, 15, 30), {1, 1, 1}}, true, {}});
        s.bodies.push_back({1, 1, .1f, {0, .2f, 0}, {0, .4f, 0}});
        s.pose = s.restPose();
        s.pose[1].r = s.pose[1].r * QQuaternion::fromEulerAngles(13, -24, 7);
        s.keys[0] = {s.pose, {}, {}};
        mj::Backend b;
        QString e;
        QVERIFY2(b.compile(s, e), qPrintable(e));
        auto d = b.data();
        QVERIFY(b.model->nq != b.model->nv);
        QVERIFY2(b.pose(s, d->qpos, e), qPrintable(e));
        mj_forward(b.model.get(), d.get());
        auto original = s.pose;
        b.reconstruct(s, d.get());
        for (int i = 0; i < 2; ++i) {
            QVERIFY((original[i].t - s.pose[i].t).length() < 1e-5);
            auto delta = original[i].r.conjugated() * s.pose[i].r;
            double angle = 2 * std::atan2(delta.vector().length(), std::abs(delta.scalar())) * 180 / M_PI;
            QVERIFY(angle <= .02);
        }
        std::vector<double> q(d->qpos, d->qpos + b.model->nq), v(b.model->nv), v2(b.model->nv);
        v[3] = .4;
        v[6] = .7;
        mj_integratePos(b.model.get(), q.data(), v.data(), .01);
        mj_differentiatePos(b.model.get(), v2.data(), .01, d->qpos, q.data());
        for (size_t i = 0; i < v.size(); ++i)
            QVERIFY(std::abs(v[i] - v2[i]) < 1e-10);
    }
    void directedAxisAndScaleDiagnostics() {
        auto s = body();
        s.nodes.push_back({"Child", 0, {{0, 1, 0}, {}, {1, 1, 1}}, true, {}});
        s.pose = s.restPose();
        s.keys[0] = {s.pose, {}, {}};
        s.bodies.push_back({1, 1, .1f, {0, .2f, 0}, {0, .4f, 0}});
        s.jointLimits[1] = {true, {0, 0, 1}, 0, -5, 70};
        mj::Backend b;
        QString e;
        QVERIFY2(b.compile(s, e), qPrintable(e));
        int j = b.bodies[1].joints[0];
        QCOMPARE(b.model->jnt_type[j], int(mjJNT_HINGE));
        QVERIFY(std::abs(b.model->jnt_range[2 * j] + 5 * M_PI / 180) < 1e-9);
        QVERIFY(std::abs(b.model->jnt_range[2 * j + 1] - 70 * M_PI / 180) < 1e-9);
        Scene baked = s;
        baked.jointLimits.clear();
        baked.rotationLimits.clear();
        baked.pose[1].r =
            (baked.nodes[1].rest.r * QQuaternion::fromAxisAndAngle(s.jointLimits[1].axis, 95)).normalized();
        QVERIFY(b.limitError(baked) > 10);
        s.nodes[0].rest.s = {1, 2, 1};
        QVERIFY(!b.compile(s, e));
        QVERIFY(e.contains("scale"));
    }
    void migrationPreservesAuthoredData() {
        auto s = Scene::mannequin();
        s.bodies = fittedPhysicsRig(s);
        s.pins[15] = {s.positions(s.pose)[15], s.defaultPole(15), 1};
        s.keys[0] = {s.pose, s.pins, {}};
        s.physics.restitution = .4f;
        s.contacts = {{19, 10, 20, s.positions(s.pose)[19], s.defaultPole(19)}};
        auto old = sceneToJson(s);
        old["version"] = 4;
        auto p = old["physics"].toObject();
        p.remove("backend");
        p.remove("migrationWarnings");
        for (QString field :
             {"backendVersion", "contactTime", "contactDamping", "externalAnchors", "requests"})
            p.remove(field);
        QJsonArray legacyBodies;
        for (auto value : p["bodies"].toArray()) {
            auto body = value.toObject();
            for (QString field : {"inertia", "inertiaRotation", "footHalfSize", "shapeRotation", "estimated",
                                  "stiffness", "damping", "maxTorque"})
                body.remove(field);
            legacyBodies.append(body);
        }
        p["bodies"] = legacyBodies;
        old.remove("physicsResult");
        QJsonObject legacyPhysics;
        for (QString key :
             {"metersPerUnit", "gravity", "ground", "friction", "restitution", "substeps", "iterations",
              "selfCollision", "followAnimation", "stiffness", "damping", "maxTorque", "bodies"})
            legacyPhysics[key] = p[key];
        QJsonArray onlyOldBodies;
        for (auto value : legacyPhysics["bodies"].toArray()) {
            auto body = value.toObject();
            QJsonObject oldBody;
            for (QString key : {"node", "mass", "radius", "center", "end"})
                oldBody[key] = body[key];
            onlyOldBodies.append(oldBody);
        }
        legacyPhysics["bodies"] = onlyOldBodies;
        old["physics"] = legacyPhysics;
        old.remove("visuals"); // Scene visuals were introduced with schema 9.
        QDir().mkpath("artifacts/physics-demo/projects");
        QFile f("artifacts/physics-demo/projects/legacy-v4.jst");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(old).toJson());
        f.close();
        QString e;
        Scene migrated;
        QVERIFY2(loadProject(f.fileName(), migrated, e), qPrintable(e));
        QCOMPARE(migrated.visuals.toJson(), SceneVisualSettings().toJson());
        QVERIFY(!migrated.physics.migrationWarnings.isEmpty());
        QCOMPARE(migrated.keys.size(), s.keys.size());
        QCOMPARE(migrated.pins[15].target, s.pins[15].target);
        QCOMPARE(migrated.physics.restitution, s.physics.restitution);
        QTemporaryDir dir;
        QVERIFY(saveProject(dir.path() + "/new.jst", migrated, e));
        Scene reopened;
        QVERIFY(loadProject(dir.path() + "/new.jst", reopened, e));
        QCOMPARE(sceneToJson(reopened)["version"].toInt(), 9);
        QCOMPARE(reopened.physics.migrationWarnings, migrated.physics.migrationWarnings);
        Scene result;
        PhysicsReport r;
        QVERIFY2(simulatePhysics(reopened, 0, 5, result, r, e), qPrintable(e));
        QCOMPARE(r.backend, physicsBackendVersion());
        QVERIFY(exportGltf(dir.path() + "/old.glb", reopened, e));
    }
    void wholeBodyBothFeetAndHand() {
        auto s = Scene::mannequin();
        s.bodies = fittedPhysicsRig(s);
        auto p = s.positions(s.pose);
        WholeBodySettings settings;
        settings.useContacts = false;
        settings.goals = {{15, 3, p[15], 1, true},
                          {19, 3, p[19], 1, true},
                          {8, 0, p[8] + QVector3D(-.15f, .15f, .2f), 1, false}};
        PhysicsReport r;
        QString e;
        QVERIFY2(solveWholeBody(s, settings, r, e), qPrintable(e));
        qInfo() << "Whole body" << r.summary();
        QVERIFY2(r.maxContactError < .002 * s.height(), qPrintable(r.summary()));
        QVERIFY2(r.maxTargetPosition < .005 * s.height(), qPrintable(r.summary()));
        settings.goals[2].target = {10, 10, 10};
        QVERIFY(solveWholeBody(s, settings, r, e));
        QCOMPARE(r.status, QString("infeasible"));
    }
    void wholeBodyCom() {
        auto s = Scene::mannequin();
        s.bodies = fittedPhysicsRig(s);
        auto p = s.positions(s.pose);
        auto com = physicalCenterOfMass(s);
        WholeBodySettings settings;
        settings.useContacts = false;
        settings.goals = {{15, 3, p[15], 1, true}, {19, 3, p[19], 1, true}};
        settings.comEnabled = true;
        settings.comTarget = com + QVector3D(.04f, 0, .025f);
        PhysicsReport r;
        QString e;
        QVERIFY2(solveWholeBody(s, settings, r, e), qPrintable(e));
        qInfo() << "COM pose" << r.summary();
        QCOMPARE(r.status, QString("solved"));
        QVERIFY(r.details["staticHintOnly"].toBool());
    }
    void portablePhysicsDemonstrations() {
        QDir().mkpath("artifacts/physics-demo/projects");
        QDir().mkpath("artifacts/physics-demo");
        QJsonArray reports;
        for (QString kind : {"whole-body", "com", "jump", "tuck", "squat", "landing"}) {
            auto source = physicsDemo(kind);
            Scene out = source;
            PhysicsReport r;
            QString error;
            QFile::remove("artifacts/physics-demo/" + kind + ".json");
            QVERIFY2(saveProject("artifacts/physics-demo/projects/" + kind + "-source.jst", source, error),
                     qPrintable(error));
            if (kind == "whole-body" || kind == "com") {
                int frame = 0;
                auto positions = source.positions(source.pose);
                QJsonObject args;
                if (kind == "whole-body")
                    args["goals"] = QJsonArray{
                        QJsonObject{{"node", 8},
                                    {"target", QJsonArray{positions[8].x() - .15, positions[8].y() + .15,
                                                          positions[8].z() + .2}},
                                    {"hard", false}}};
                else {
                    auto com = physicalCenterOfMass(source);
                    args["comTarget"] = QJsonArray{com.x() + .04, com.y(), com.z() + .025};
                }
                auto report = EditService::apply(out, frame, "whole_body", args);
                out.physicsResult = report;
                QVERIFY2(report["status"] == "solved",
                         qPrintable(QString::fromUtf8(QJsonDocument(report).toJson())));
                reports.append(QJsonObject{{"demo", kind}, {"report", report}});
            } else {
                if (kind == "jump" || kind == "tuck")
                    QVERIFY2(solveFlight(source, 0, source.last, FlightSettings{}, out, r, error),
                             qPrintable(error));
                else {
                    DynamicsSettings settings;
                    settings.iterations = 30;
                    settings.seconds = kind == "landing" ? 40 : 20;
                    settings.importantFrames = {{0, 10}, {15, 5}, {30, 10}};
                    settings.endAtRest = kind == "landing";
                    QVERIFY2(optimizeDynamics(source, 0, source.last, settings, out, r, error),
                             qPrintable(error));
                }
                if (kind == "jump" || kind == "tuck")
                    QVERIFY2(r.status == "solved", qPrintable(r.summary()));
                reports.append(QJsonObject{{"demo", kind}, {"report", r.json()}});
            }
            QVERIFY2(saveProject("artifacts/physics-demo/projects/" + kind + "-result.jst", out, error),
                     qPrintable(error));
            const QString folder = "artifacts/physics-demo/" + kind;
            QDir().mkpath(folder);
            QVERIFY2(exportGltf(folder + "/result.glb", out, error), qPrintable(error));
            QVERIFY2(exportFbx(folder + "/result.fbx", out, error), qPrintable(error));
            QVERIFY2(writeExportReference(folder, out, error), qPrintable(error));
            QFile rfile("artifacts/physics-demo/" + kind + ".json");
            QVERIFY(rfile.open(QIODevice::WriteOnly));
            rfile.write(QJsonDocument(out.physicsResult).toJson());
            QString xml;
            if (out.physicsResult.contains("controlReplay"))
                xml = out.physicsResult["controlReplay"].toObject()["modelXml"].toString();
            else
                QVERIFY2(physicsModelXml(source, xml, error, false), qPrintable(error));
            QFile model("artifacts/physics-demo/" + kind + ".xml");
            QVERIFY(model.open(QIODevice::WriteOnly));
            model.write(xml.toUtf8());
            if (kind == "squat" || kind == "landing") {
                // Native controls are feasible, while fixed baked-inverse gates remain deliberately
                // unmet.
                QCOMPARE(r.status, QString("approximate"));
                QVERIFY2(r.details["nativeForwardVerified"].toBool(), qPrintable(r.summary()));
                QVERIFY(!r.details["bakedInverseVerified"].toBool());
                QVERIFY(r.details["independentReplay"].toObject()["momentumBalancePassed"].toBool());
                QVERIFY(r.maxContactError <= .01 * source.height() * source.physics.metersPerUnit);
                QVERIFY(r.maxTargetPosition <= .02 * source.height() * source.physics.metersPerUnit);
                QVERIFY(r.maxTargetAngle <= 5);
                QVERIFY(r.details["unexpectedFlightContactN"].toDouble() <=
                        .01 * physicalMass(source) * source.physics.gravity);
            }
        }
        QFile summary("artifacts/physics-demo/reports.json");
        QVERIFY(summary.open(QIODevice::WriteOnly));
        summary.write(QJsonDocument(reports).toJson());
    }
    void boundedTrajectoryOptimization() {
        auto s = body();
        s.last = 18;
        s.physics.gravity = 0;
        s.physics.ground = -100;
        s.physics.substeps = 8;
        s.nodes.push_back({"Child", 0, {{0, .5f, 0}, {}, {1, 1, 1}}, true, {}});
        s.bodies.push_back({1, 1, .07f, {0, .15f, 0}, {0, .3f, 0}});
        for (int f = 0; f <= 18; ++f) {
            s.pose = s.restPose();
            float t = f / 18.f;
            s.pose[1].r = QQuaternion::fromAxisAndAngle(0, 0, 1, 30 * std::sin(t * M_PI));
            if (f > 1 && f < 17)
                s.pose[0].t.setX(.02f * std::pow(std::sin((f - 1) * M_PI / 16), 2));
            s.keys[f] = {s.pose, {}, {}};
        }
        s.evaluate(0);
        DynamicsSettings settings;
        settings.iterations = 8;
        settings.seconds = 10;
        Scene out;
        PhysicsReport r;
        QString e;
        QVERIFY2(optimizeDynamics(s, 0, 18, settings, out, r, e), qPrintable(e));
        qInfo() << "Optimization" << r.summary();
        QVERIFY(r.details["finalObjective"].toDouble() < r.details["initialObjective"].toDouble());
        auto pair = r.details["sampledAnimationInverse"].toObject();
        auto before = pair["before"].toObject(), after = pair["after"].toObject();
        QCOMPARE(before["methodVersion"], after["methodVersion"]);
        QCOMPARE(before["analysisFps"], after["analysisFps"]);
        QCOMPARE(before["samples"], after["samples"]);
        QCOMPARE(after["rootForceRmsN"], r.details["afterRootForceRmsN"]);
        qInfo() << "Common-grid inverse force" << before["rootForceRmsN"] << after["rootForceRmsN"];
        QVERIFY(after["rootForceRmsN"].toDouble() < .7 * before["rootForceRmsN"].toDouble());
        // This source requests angular motion in zero gravity without compensating root motion.
        // A finite search must report its target conflict rather than silently certify the result.
        QVERIFY(r.maxTargetPosition <= .02 * s.height() || !r.details["nativeForwardVerified"].toBool());
        if(r.maxTargetPosition > .02*s.height()) QCOMPARE(r.status, QString("approximate"));
        QVERIFY(r.details["forwardReplayTangentPeak"].toDouble() < 1e-6);
        QVERIFY(!r.details["externalAnchors"].toBool());
        QCOMPARE(r.details["rootActuators"].toInt(), 0);
        PhysicsControl cancel;
        cancel.cancel = true;
        QVERIFY(!optimizeDynamics(s, 0, 18, settings, out, r, e, &cancel));
    }
    void flightBoundaryAndTuck() {
        auto s = body();
        s.keys[60] = s.keys[0];
        FlightSettings f;
        Scene out;
        PhysicsReport r;
        QString e;
        f.constrainEndVelocity = true;
        f.endVelocity = {0, 0, 0};
        QVERIFY(!solveFlight(s, 0, 60, f, out, r, e));
        QVERIFY(e.contains("Incompatible"));
        f.constrainEndVelocity = false;
        QVERIFY2(solveFlight(s, 0, 60, f, out, r, e), qPrintable(e));
        QVERIFY(r.maxBallisticError < 1e-4 * s.height());
        auto character = Scene::mannequin();
        character.bodies = fittedPhysicsRig(character);
        character.physics.ground = -100;
        character.last = 60;
        for (int frame = 0; frame <= 60; ++frame) {
            character.pose = character.restPose();
            character.pose[0].t += {0, 5, 0};
            character.pose[0].r = QQuaternion::fromAxisAndAngle(0, 0, 1, frame * 3.f);
            float tuck = std::sin(frame * M_PI / 60);
            for (int n : {6, 10})
                character.pose[n].r = QQuaternion::fromAxisAndAngle(0, 0, 1, (n == 6 ? -1 : 1) * 80 * tuck);
            character.keys[frame] = {character.pose, {}, {}};
        }
        character.evaluate(0);
        QVERIFY2(solveFlight(character, 0, 60, f, out, r, e), qPrintable(e));
        qInfo() << "Tuck flight" << r.summary();
        QVERIFY(r.maxBallisticError < 1e-4 * character.height());
        QVERIFY2(r.angularMomentumDrift < .01, qPrintable(r.summary()));
    }
};
QTEST_GUILESS_MAIN(MuJoCoTests)
#include "mujoco_tests.moc"
