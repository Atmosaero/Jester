#include "io/project/project_io.h"
#include "core/scene/core.h"
#include "core/scene/document.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace trickster;
class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void hierarchyAndSkin() {
        auto s = Scene::mannequin();
        QString error;
        QVERIFY2(s.validate(&error), qPrintable(error));
        auto p = s.positions(s.pose);
        QVERIFY((p[8] - QVector3D(.86f, 1.48f, 0)).length() < .001f);
        auto skinned = s.skinned(s.meshes[0]);
        for (size_t i = 0; i < skinned.size(); ++i)
            QVERIFY((skinned[i] - s.meshes[0].vertices[i].p).length() < .00001f);
        s.translateWorld(0, {.2f, 0, 0});
        auto moved = s.skinned(s.meshes[0]);
        for (size_t i = 0; i < moved.size(); ++i)
            QVERIFY((moved[i] - skinned[i] - QVector3D(.2f, 0, 0)).length() < .00001f);
    }
    void ikAndPins() {
        auto s = Scene::mannequin();
        auto p = s.positions(s.pose);
        for (int n : {15, 19})
            s.pins[n] = {p[n], s.defaultPole(n), 1};
        s.translateWorld(0, {.035f, -.18f, .02f});
        auto posed = s.positions(s.pose);
        for (int n : {15, 19})
            QVERIFY2((posed[n] - p[n]).length() < .0001f, "Pinned foot moved");
        QVector3D goal = p[8] + QVector3D(-.2f, .15f, .2f);
        QVERIFY(s.solveIK(8, goal, s.defaultPole(8)));
        QVERIFY((s.positions(s.pose)[8] - goal).length() < .0001f);
        auto root = s.positions(s.pose)[6];
        s.solveIK(8, root + QVector3D(100, 0, 0), s.defaultPole(8));
        QVERIFY((s.positions(s.pose)[8] - root).length() < .57f);
        QVERIFY(s.validate());
    }
    void poleDirection() {
        auto s = Scene::mannequin();
        auto p = s.positions(s.pose);
        QVector3D target = p[8] + QVector3D(-.2f, 0, 0);
        s.solveIK(8, target, p[6] + QVector3D(0, 0, 1));
        float a = s.positions(s.pose)[7].z();
        s.pose = s.restPose();
        s.solveIK(8, target, p[6] + QVector3D(0, 0, -1));
        float b = s.positions(s.pose)[7].z();
        QVERIFY(a > .05f);
        QVERIFY(b < -.05f);
    }
    void interpolation() {
        auto s = Scene::mannequin();
        s.keys[0] = {s.pose, {}};
        s.pose[0].t.setX(1);
        s.pose[4].r = QQuaternion::fromAxisAndAngle(0, 1, 0, 90);
        s.keys[30] = {s.pose, {}};
        s.evaluate(15);
        QVERIFY(std::abs(s.pose[0].t.x() - .5f) < 1e-6f);
        QVERIFY(std::abs(s.pose[4].r.toEulerAngles().y() - 45) < .001f);
    }
    void workflowRoundTrip() {
        Document d;
        d.demo();
        QTemporaryDir dir;
        QString error;
        QVERIFY(saveProject(dir.path() + "/demo.jst", d.scene, error));
        Scene reopened;
        QVERIFY2(loadProject(dir.path() + "/demo.jst", reopened, error), qPrintable(error));
        QCOMPARE(reopened.keys.size(), 4);
        for (const QString ext : {"glb", "gltf"}) {
            QString path = dir.path() + "/animation." + ext;
            QVERIFY2(exportGltf(path, reopened, error), qPrintable(error));
            Scene imported;
            QVERIFY2(loadGltf(path, imported, error), qPrintable(error));
            QCOMPARE(imported.meshes.size(), reopened.meshes.size());
            for (int f : {0, 15, 30, 45, 60, 75, 90}) {
                reopened.evaluate(float(f));
                imported.evaluate(float(f));
                auto a = reopened.positions(reopened.pose), b = imported.positions(imported.pose);
                for (size_t i = 0; i < a.size(); ++i)
                    QVERIFY2((a[i] - b[i]).length() < .0001f,
                             qPrintable(QString("Joint %1 frame %2 mismatch").arg(i).arg(f)));
                auto ma = reopened.skinned(reopened.meshes[0]), mb = imported.skinned(imported.meshes[0]);
                for (size_t i = 0; i < ma.size(); ++i)
                    QVERIFY((ma[i] - mb[i]).length() < .0002f);
            }
        }
    }
    void undoKeys() {
        Document d;
        d.replace(Scene::mannequin());
        d.insertKey();
        d.seek(30);
        d.edit("Move hips", [](Scene &s) { s.translateWorld(0, {0, -.1f, 0}); });
        QCOMPARE(d.scene.keys.size(), 2);
        auto p = d.scene.pose[0].t;
        d.undo.undo();
        QCOMPARE(d.scene.keys.size(), 1);
        d.undo.redo();
        QCOMPARE(d.scene.pose[0].t, p);
        QVERIFY(d.moveKey(30, 45));
        QVERIFY(!d.scene.keys.contains(30));
        QVERIFY(d.moveKey(45, 60, true));
        QCOMPARE(d.scene.keys.size(), 3);
        d.removeKey(45);
        QVERIFY(!d.scene.keys.contains(45));
        d.undo.undo();
        QVERIFY(d.scene.keys.contains(45));
    }
    void scaledHierarchyIK() {
        auto s = Scene::mannequin();
        s.pose[0].s = {.01f, .01f, .01f};
        s.pose[0].r = QQuaternion::fromEulerAngles(25, 70, -10);
        auto p = s.positions(s.pose);
        auto goal = p[8] + QVector3D(-.001f, .001f, .001f);
        QVERIFY(s.solveIK(8, goal, s.defaultPole(8)));
        QVERIFY((s.positions(s.pose)[8] - goal).length() < .00001f);
    }
    void externalCharacter() {
        QString path = "artifacts/external/CesiumMan.glb";
        if (!QFileInfo::exists(path))
            QSKIP("Optional official Khronos fixture is absent");
        Scene s;
        QString error;
        QVERIFY2(loadGltf(path, s, error), qPrintable(error));
        QVERIFY(s.nodes.size() > 10);
        QVERIFY(!s.meshes.empty());
        QVERIFY(!s.keys.isEmpty());
        QVERIFY(s.validate());
        QVERIFY2(exportGltf("artifacts/external/CesiumMan-export.glb", s, error), qPrintable(error));
        Scene other;
        QVERIFY2(loadGltf("artifacts/external/CesiumMan-export.glb", other, error), qPrintable(error));
        for (int f : {0, 15, 30}) {
            s.evaluate(float(f));
            other.evaluate(float(f));
            auto a = s.skinned(s.meshes[0]), b = other.skinned(other.meshes[0]);
            QCOMPARE(a.size(), b.size());
            for (size_t i = 0; i < a.size(); ++i)
                QVERIFY((a[i] - b[i]).length() < .0005f);
        }
    }
    void helpersAndCopies() {
        auto s = Scene::mannequin();
        auto p = s.positions(s.pose);
        for (int n : {15, 19})
            s.pins[n] = {p[n], s.defaultPole(n), 1};
        s.keys[0] = {s.pose, s.pins};
        s.translateWorld(0, {0, -.1f, 0});
        s.solveIK(8, p[8] + QVector3D(-.2f, .1f, .2f), s.defaultPole(8));
        s.keys[30] = {s.pose, s.pins};
        s.last = 30;
        auto before = s.pose;
        s.mirrorPose();
        QVERIFY((s.positions(s.pose)[12] - QVector3D(-s.positions(before)[8].x(), s.positions(before)[8].y(),
                                                     s.positions(before)[8].z()))
                    .length() < .001f);
        s.mirrorPose();
        for (size_t i = 0; i < s.pose.size(); ++i)
            QVERIFY(std::abs(QQuaternion::dotProduct(before[i].r, s.pose[i].r)) > .9999f);
        auto b = s.balance();
        QVERIFY(b.support.size() >= 3);
        QVERIFY(std::isfinite(b.center.y()));
        QVERIFY(b.supported);
        s.rotationLimits[4] = 25;
        s.pose[4].r = QQuaternion::fromAxisAndAngle(0, 1, 0, 90);
        s.enforceLimits();
        QVERIFY(std::abs(s.pose[4].r.toEulerAngles().y() - 25) < .001f);
        s.keys[30] = {s.pose, s.pins};
        auto baked = s.bakedCopy();
        QCOMPARE(baked.keys.size(), 31);
        QVERIFY(baked.pins.isEmpty());
        for (int f : {0, 15, 30}) {
            s.evaluate(float(f));
            baked.evaluate(float(f));
            auto a = s.positions(s.pose), bb = baked.positions(baked.pose);
            for (size_t i = 0; i < a.size(); ++i)
                QVERIFY((a[i] - bb[i]).length() < .0001f);
        }
        auto jump = s.ballisticCopy(0, 30, .4f);
        s.evaluate(0);
        auto c0 = s.balance().center;
        s.evaluate(30);
        auto c1 = s.balance().center;
        jump.evaluate(15);
        QVERIFY((jump.balance().center - (c0 + c1) * .5f - QVector3D(0, .4f, 0)).length() < .0001f);
        QCOMPARE(s.keys.size(), 2);
        QVERIFY(jump.validate());
    }
    void contactsAndPersistence() {
        auto s = Scene::mannequin();
        auto rest = s.positions(s.pose);
        s.keys[0] = {s.pose, {}};
        s.translateWorld(0, {0, -.15f, .02f});
        s.keys[30] = {s.pose, {}};
        s.contacts.push_back({15, 0, 30, rest[15], s.defaultPole(15)});
        s.contacts.push_back({19, 0, 30, rest[19], s.defaultPole(19)});
        s.bodyAssist = true;
        s.rotationLimits[4] = 45;
        for (int f : {0, 10, 20, 30}) {
            s.evaluate(float(f));
            auto p = s.positions(s.pose);
            for (int n : {15, 19})
                QVERIFY((p[n] - rest[n]).length() < .0001f);
        }
        s.evaluate(20);
        QString error;
        Scene other;
        QVERIFY(sceneFromJson(sceneToJson(s), other, error));
        QCOMPARE(other.contacts.size(), 2);
        QVERIFY(other.bodyAssist);
        QCOMPARE(other.rotationLimits[4], 45.f);
        QCOMPARE(other.sampleFrame, 20.f);
        Document d;
        d.replace(other);
        QCOMPARE(d.frame, 20);
        QTemporaryDir dir;
        QVERIFY(exportGltf(dir.path() + "/contacts.glb", s, error));
        Scene imported;
        QVERIFY(loadGltf(dir.path() + "/contacts.glb", imported, error));
        for (int f : {10, 20, 30}) {
            imported.evaluate(float(f));
            for (int n : {15, 19})
                QVERIFY((imported.positions(imported.pose)[n] - rest[n]).length() < .0002f);
        }
    }
    void bodyAssistReach() {
        auto s = Scene::mannequin();
        auto p = s.positions(s.pose);
        for (int n : {15, 19})
            s.pins[n] = {p[n], s.defaultPole(n), 1};
        s.translateWorld(0, {0, -.12f, 0});
        auto rest = s.pose;
        auto target = s.positions(s.pose)[8] + QVector3D(.2f, .2f, 0);
        s.solveIK(8, target, s.defaultPole(8));
        float plain = (s.positions(s.pose)[8] - target).length();
        s.pose = rest;
        s.bodyAssist = true;
        s.reach(8, target, s.defaultPole(8));
        QVERIFY((s.positions(s.pose)[8] - target).length() < plain);
        for (int n : {15, 19})
            QVERIFY((s.positions(s.pose)[n] - p[n]).length() < .001f);
    }
    void frameRateAndRange() {
        auto s = Scene::mannequin();
        s.fps = 60;
        s.first = 15;
        s.last = 45;
        s.keys[15] = {s.pose, {}};
        s.pose[0].t.setX(1);
        s.keys[45] = {s.pose, {}};
        QTemporaryDir dir;
        QString error;
        QVERIFY(exportGltf(dir.path() + "/rate.glb", s, error));
        Scene imported;
        QVERIFY(loadGltf(dir.path() + "/rate.glb", imported, error));
        QCOMPARE(imported.fps, 60);
        QCOMPARE(imported.last, 30);
        imported.evaluate(15);
        QVERIFY(std::abs(imported.pose[0].t.x() - .5f) < .0001f);
    }

    void importedInterpolationAndErrors() {
        QTemporaryDir dir;
        QString error;
        for (QString mode : {"LINEAR", "STEP", "CUBICSPLINE"}) {
            std::vector<float> times = {0, 1}, values = mode == "CUBICSPLINE"
                                                            ? std::vector<float>{0, 0, 0, 0, 0, 0, 4, 0, 0,
                                                                                 0, 0, 0, 2, 0, 0, 0, 0, 0}
                                                            : std::vector<float>{0, 0, 0, 2, 0, 0};
            QByteArray bytes;
            bytes.append(reinterpret_cast<const char *>(times.data()), 8);
            bytes.append(reinterpret_cast<const char *>(values.data()), qsizetype(values.size() * 4));
            QJsonObject root{
                {"asset", QJsonObject{{"version", "2.0"}}},
                {"nodes", QJsonArray{QJsonObject{{"name", "Root"}}}},
                {"buffers", QJsonArray{QJsonObject{{"byteLength", bytes.size()},
                                                   {"uri", "data:application/octet-stream;base64," +
                                                               QString::fromLatin1(bytes.toBase64())}}}},
                {"bufferViews",
                 QJsonArray{
                     QJsonObject{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 8}},
                     QJsonObject{{"buffer", 0}, {"byteOffset", 8}, {"byteLength", int(values.size() * 4)}}}},
                {"accessors",
                 QJsonArray{QJsonObject{
                                {"bufferView", 0}, {"componentType", 5126}, {"count", 2}, {"type", "SCALAR"}},
                            QJsonObject{{"bufferView", 1},
                                        {"componentType", 5126},
                                        {"count", int(values.size() / 3)},
                                        {"type", "VEC3"}}}},
                {"animations",
                 QJsonArray{QJsonObject{
                     {"samplers",
                      QJsonArray{QJsonObject{{"input", 0}, {"output", 1}, {"interpolation", mode}}}},
                     {"channels", QJsonArray{QJsonObject{
                                      {"sampler", 0},
                                      {"target", QJsonObject{{"node", 0}, {"path", "translation"}}}}}}}}}};
            QString file = dir.path() + "/interpolation.gltf";
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(root).toJson());
            f.close();
            Scene s;
            QVERIFY2(loadGltf(file, s, error), qPrintable(error));
            s.evaluate(15);
            QVERIFY(std::abs(s.pose[0].t.x() - (mode == "LINEAR" ? 1.f
                                                : mode == "STEP" ? 0.f
                                                                 : 1.5f)) < .0001f);
            root["extensionsRequired"] = QJsonArray{"KHR_draco_mesh_compression"};
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(root).toJson());
            f.close();
            QVERIFY(!loadGltf(file, s, error));
            QVERIFY(error.contains("Unsupported required extension"));
        }
        QFile truncated(dir.path() + "/bad.glb");
        QVERIFY(truncated.open(QIODevice::WriteOnly));
        truncated.write("glTF", 4);
        truncated.close();
        Scene untouched = Scene::mannequin();
        QVERIFY(!loadGltf(truncated.fileName(), untouched, error));
        QCOMPARE(untouched.nodes.size(), size_t(21));
        QVERIFY(!saveProject(dir.path() + "/absent/project.jst", untouched, error));
    }

    void softPinStability() {
        auto s = Scene::mannequin();
        auto p = s.positions(s.pose);
        s.pins[8] = {p[8] + QVector3D(-.2f, .15f, .15f), s.defaultPole(8), .5f};
        s.solvePins();
        s.keys[0] = {s.pose, s.pins};
        s.keys[30] = {s.pose, s.pins};
        auto authored = s.positions(s.pose);
        s.evaluate(0);
        QVERIFY((s.positions(s.pose)[8] - authored[8]).length() < .0001f);
        s.evaluate(.001f);
        QVERIFY((s.positions(s.pose)[8] - authored[8]).length() < .001f);
        QTemporaryDir dir;
        QString error;
        QVERIFY(exportGltf(dir.path() + "/soft.glb", s, error));
        Scene roundTrip;
        QVERIFY(loadGltf(dir.path() + "/soft.glb", roundTrip, error));
        roundTrip.evaluate(0);
        QVERIFY((roundTrip.positions(roundTrip.pose)[8] - authored[8]).length() < .0001f);
    }

    void emptySceneAndSessionHistory() {
        Document d;
        QVERIFY(d.scene.nodes.empty());
        QCOMPARE(d.selected, -1);
        QVERIFY(d.scene.validate());
        d.insertKey();
        QVERIFY(d.scene.keys.isEmpty());
        d.play(true);
        QVERIFY(!d.playing);
        QTemporaryDir dir;
        QString error;
        QVERIFY(saveProject(dir.path() + "/empty.jst", d.scene, error));
        Scene opened;
        QVERIFY(loadProject(dir.path() + "/empty.jst", opened, error));
        QVERIFY(opened.nodes.empty());
        QVERIFY(!exportGltf(dir.path() + "/empty.glb", opened, error));
        d.replace(Scene::mannequin());
        d.edit("A", [](Scene &s) { s.translateWorld(0, {.2f, 0, 0}); });
        d.undo.setClean();
        d.edit("B", [](Scene &s) { s.translateWorld(0, {.2f, 0, 0}); });
        d.undo.undo();
        d.select(8);
        d.seek(15);
        auto saved = d.captureSession();
        d.replace(Scene{});
        d.restoreSession(saved);
        QCOMPARE(d.frame, 15);
        QCOMPARE(d.selected, 8);
        QVERIFY(!d.dirty());
        QVERIFY(d.undo.canRedo());
        QVERIFY(std::abs(d.scene.pose[0].t.x() - .2f) < .0001f);
        d.undo.redo();
        QVERIFY(d.dirty());
        QVERIFY(std::abs(d.scene.pose[0].t.x() - .4f) < .0001f);
        d.undo.undo();
        QVERIFY(!d.dirty());
        d.undo.undo();
        QVERIFY(std::abs(d.scene.pose[0].t.x()) < .0001f);
    }
    void suppliedCharacterClips() {
        QDir folder("library/TestCharacter");
        if (!folder.exists())
            QSKIP("Local user character library absent");
        auto files = folder.entryList({"*.glb"}, QDir::Files);
        QCOMPARE(files.size(), 21);
        for (const auto &file : files) {
            Scene s;
            QString error;
            QVERIFY2(loadGltf(folder.filePath(file), s, error), qPrintable(file + ": " + error));
            QVERIFY(!s.meshes.empty());
            QVERIFY(s.nodes.size() > 40);
            QVERIFY(s.validate(&error));
            if (file != "TestCharacter_Rest.glb")
                QVERIFY(!s.keys.isEmpty());
        }
    }
    void rejectsInvalidProject() {
        auto s = Scene::mannequin();
        QString error;
        auto j = sceneToJson(s);
        j["version"] = 999;
        Scene unchanged = s;
        QVERIFY(!sceneFromJson(j, unchanged, error));
        QCOMPARE(unchanged.nodes.size(), s.nodes.size());
        s.nodes[0].parent = 4;
        QVERIFY(!s.validate(&error));
    }
};
QTEST_MAIN(CoreTests)
#include "core_tests.moc"
