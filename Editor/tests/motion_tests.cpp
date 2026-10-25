#include "settings/appearance.h"
#include "core/scene/core.h"
#include "core/scene/document.h"
#include "automation/editor_api.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace trickster;
namespace {
Scene denseMotion() {
    auto s = Scene::mannequin();
    s.last = 60;
    for (int f = 0; f <= s.last; ++f) {
        s.pose = s.restPose();
        s.pose[0].t.setX(.004f * f);
        s.pose[6].r = QQuaternion::fromEulerAngles(0, 0, 8 * std::sin(float(f) / 20));
        s.keys[f] = {s.pose, {}, s.morphs};
    }
    s.evaluate(0);
    return s;
}
void comparePose(const Pose &a, const Pose &b, float tolerance = 1e-5f) {
    QCOMPARE(a.size(), b.size());
    for (size_t n = 0; n < a.size(); ++n) {
        QVERIFY((a[n].t - b[n].t).length() < tolerance);
        QVERIFY((a[n].s - b[n].s).length() < tolerance);
        QVERIFY(std::abs(QQuaternion::dotProduct(a[n].r, b[n].r)) > 1 - tolerance);
    }
}
} // namespace
class MotionTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        applyAppearance(*qobject_cast<QApplication *>(QApplication::instance()));
    }
    void reproduceIsolatedKeyEdit() {
        Document d;
        d.replace(denseMotion());
        d.seek(30);
        d.edit("Legacy wrist edit", [](Scene &s) { s.pose[8].t.setY(s.pose[8].t.y() + .1f); });
        d.seek(29);
        auto before = d.scene.positions(d.scene.pose)[8];
        d.seek(30);
        auto edited = d.scene.positions(d.scene.pose)[8];
        d.seek(31);
        auto after = d.scene.positions(d.scene.pose)[8];
        QVERIFY((edited - (before + after) * .5f).length() > .09f);
    }
    void smoothCorrectionAndExactDisable() {
        Document d;
        auto source = denseMotion();
        d.replace(source);
        QString error;
        d.edit(
            "Correction", [&](Scene &s) { QVERIFY(s.beginCorrection(10, 50, 10, 10, false, error)); }, false);
        d.seek(30);
        auto before = d.scene.pose;
        d.edit("Lift wrist", [](Scene &s) { s.pose[8].t.setY(s.pose[8].t.y() + .1f); });
        QCOMPARE(d.scene.keys.size(), 61);
        QCOMPARE(d.scene.sampleLayer(d.scene.correction(),float(d.frame)).offsets.size(), 1);
        QVERIFY((d.scene.pose[8].t - before[8].t - QVector3D(0, .1f, 0)).length() < 1e-5f);
        for (float f : {0.f, 10.f, 10.1f, 15.f, 29.f, 30.f, 31.f, 45.f, 49.9f, 50.f, 60.f}) {
            d.scene.evaluate(f);
            source.evaluate(f);
            float expected = .1f * d.scene.correctionWeight(f);
            QVERIFY(std::abs(d.scene.pose[8].t.y() - source.pose[8].t.y() - expected) < 1e-5f);
            auto result = d.scene.pose;
            d.scene.evaluate(1);
            d.scene.evaluate(59);
            d.scene.evaluate(f);
            comparePose(d.scene.pose, result);
            d.scene.correction().enabled = false;
            d.scene.evaluate(f);
            comparePose(d.scene.pose, source.pose);
            d.scene.correction().enabled = true;
        }
        auto corrected = d.scene.correction();
        d.undo.undo();
        QVERIFY(d.scene.correction().offsets.empty());
        d.undo.redo();
        QCOMPARE(d.scene.sampleLayer(d.scene.correction(),float(d.frame)).offsets.size(), d.scene.sampleLayer(corrected,float(d.frame)).offsets.size());
        d.begin();
        d.scene.pose[8].t.setY(50);
        d.cancel();
        QVERIFY(d.scene.pose[8].t.y() < 1);
    }
    void rotationMorphAndWorldSpace() {
        auto s = denseMotion();
        s.nodes[8].morphRest = {0};
        s.morphs[8] = {0};
        for (auto &k : s.keys)
            k.morphs[8] = {0};
        QString error;
        QVERIFY(s.beginCorrection(0, 60, 10, 10, true, error));
        s.evaluate(30);
        auto old = s.pose;
        auto weights = s.morphs;
        s.rotate(8, {0, 1, 0}, 35, true);
        s.translateWorld(8, {0, .12f, .02f});
        s.morphs[8] = {.6f};
        auto desired = s.pose;
        QVERIFY2(s.captureCorrection(old, weights, error), qPrintable(error));
        comparePose(s.pose, desired);
        s.evaluate(15);
        QVERIFY(std::abs(s.morphWeights(8)[0] - .6f) < 1e-5f);
        s.evaluate(0);
        QCOMPARE(s.morphWeights(8)[0], 0.f);
        s.correction().enabled = false;
        s.evaluate(30);
        QCOMPARE(s.morphWeights(8)[0], 0.f);
        // Quaternion sign must not change the correction.
        s.correction().enabled = true;
        s.evaluate(30);
        auto result = s.pose;
        for(auto key=s.correction().keys.begin();key!=s.correction().keys.end();++key)key->offsets[8].r = -key->offsets[8].r;
        s.evaluate(30);
        comparePose(s.pose, result);
    }
    void subDegreeCorrection() {
        auto s = denseMotion();
        QString error;
        QVERIFY(s.beginCorrection(0, 60, 10, 10, false, error));
        s.evaluate(30);
        auto old = s.pose;
        s.rotate(8, {1, 0, 0}, .01f, false);
        QVERIFY(s.captureCorrection(old, s.morphs, error));
        QVERIFY(s.sampleLayer(s.correction(),s.sampleFrame).offsets.contains(8));
        QVERIFY((s.pose[8].r.vector() - old[8].r.vector()).length() > .00005f);
    }
    void largeRotationAtPartialInfluenceIsDiagnosed() {
        auto s = denseMotion();
        QString error;
        QVERIFY(s.beginCorrection(0, 60, 10, 10, false, error));
        s.correction().strength = .5f;
        s.evaluate(30);
        auto old = s.pose;
        s.rotate(8, {0, 1, 0}, 110, false);
        QVERIFY(!s.captureCorrection(old, s.morphs, error));
        QVERIFY(error.contains("full influence"));
        QVERIFY(s.correction().offsets.empty());
        s.correction().strength = 1;
        s.evaluate(30);
        old = s.pose;
        s.rotate(8, {0, 1, 0}, 110, false);
        auto desired = s.pose;
        QVERIFY(s.captureCorrection(old, s.morphs, error));
        comparePose(s.pose, desired);
    }
    void ikAffectsOnlyChainAndKeepsSource() {
        auto s = denseMotion();
        auto keys = s.keys;
        QString error;
        QVERIFY(s.beginCorrection(0, 60, 10, 10, false, error));
        s.evaluate(30);
        auto old = s.pose;
        auto target = s.positions(s.pose)[8] + QVector3D(-.15f, .08f, .1f);
        QVERIFY(s.solveIK(8, target, s.defaultPole(8)));
        QVERIFY2(s.captureCorrection(old, s.morphs, error), qPrintable(error));
        QVERIFY(s.sampleLayer(s.correction(),s.sampleFrame).offsets.contains(6));
        QVERIFY(s.sampleLayer(s.correction(),s.sampleFrame).offsets.contains(7));
        QVERIFY(!s.sampleLayer(s.correction(),s.sampleFrame).offsets.contains(15));
        QVERIFY((s.positions(s.pose)[8] - target).length() < 1e-4f);
        for (auto it = keys.begin(); it != keys.end(); ++it)
            comparePose(it->pose, s.keys[it.key()].pose);
    }
    void projectExportAndBake() {
        auto s = denseMotion();
        QString error;
        QVERIFY(s.beginCorrection(0, 60, 12, 12, false, error));
        s.evaluate(30);
        auto old = s.pose;
        s.rotate(7, {0, 0, 1}, 20, false);
        QVERIFY(s.captureCorrection(old, s.morphs, error));
        QTemporaryDir dir;
        QVERIFY(saveProject(dir.filePath("edit.trickster"), s, error));
        Scene project;
        QVERIFY2(loadProject(dir.filePath("edit.trickster"), project, error), qPrintable(error));
        QVERIFY(project.correction().present);
        auto baked = s.bakedCopy();
        QVERIFY(!baked.correction().present);
        for (auto ext : {"glb", "fbx"}) {
            auto path = dir.filePath(QString("edit.") + ext);
            QVERIFY2(exportAnimation(path, s, error), qPrintable(error));
            Scene loaded;
            QVERIFY2(loadCharacter(path, loaded, error), qPrintable(error));
            for (float f : {0.f, 6.f, 15.f, 30.f, 45.f, 54.f, 60.f}) {
                s.evaluate(f);
                project.evaluate(f);
                baked.evaluate(f);
                loaded.evaluate(f);
                comparePose(s.pose, project.pose);
                comparePose(s.pose, baked.pose);
                auto a = s.positions(s.pose), b = loaded.positions(loaded.pose);
                for (size_t n = 0; n < a.size(); ++n) {
                    int mapped = -1;
                    for (int j = 0; j < int(loaded.nodes.size()); ++j)
                        if (loaded.nodes[j].name == s.nodes[n].name)
                            mapped = j;
                    QVERIFY(mapped >= 0);
                    QVERIFY2((a[n] - b[mapped]).length() < .001f,
                             qPrintable(QString("%1 frame %2 node %3 error %4")
                                            .arg(ext)
                                            .arg(f)
                                            .arg(s.nodes[n].name)
                                            .arg((a[n] - b[mapped]).length())));
                }
                auto x = s.skinned(s.meshes[0]), y = loaded.skinned(loaded.meshes[0]);
                // GLB preserves topology; FBX triangulation may split vertices.
                if (QString(ext) == "glb") {
                    QCOMPARE(x.size(), y.size());
                    for (size_t v = 0; v < x.size(); ++v)
                        QVERIFY((x[v] - y[v]).length() < .001f);
                }
            }
        }
        auto json = sceneToJson(s);
        json["version"] = 2;
        json.remove("correction");
        Scene legacy;
        QVERIFY(sceneFromJson(json, legacy, error));
        QVERIFY(!legacy.correction().present);
        json = sceneToJson(s);
        auto savedLayers=json["layers"].toArray();auto c = savedLayers[0].toObject();
        c["fadeIn"] = 999;
        savedLayers[0]=c;json["layers"] = savedLayers;
        QVERIFY(!sceneFromJson(json, legacy, error));
    }
    void nativeTimingAndClipSelection() {
        QByteArray data;
        auto add = [&](std::initializer_list<float> values) {
            for (float v : values)
                data.append(reinterpret_cast<const char *>(&v), 4);
        };
        add({0, .13f, .47f});
        add({0, 0, 0, 1, 0, 0, 2, 0, 0});
        QJsonObject root{
            {"asset", QJsonObject{{"version", "2.0"}}},
            {"nodes", QJsonArray{QJsonObject{{"name", "Joint"}}}},
            {"buffers", QJsonArray{QJsonObject{{"byteLength", data.size()},
                                               {"uri", "data:application/octet-stream;base64," +
                                                           QString::fromLatin1(data.toBase64())}}}},
            {"bufferViews", QJsonArray{QJsonObject{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 12}},
                                       QJsonObject{{"buffer", 0}, {"byteOffset", 12}, {"byteLength", 36}}}},
            {"accessors",
             QJsonArray{
                 QJsonObject{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "SCALAR"}},
                 QJsonObject{{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}}}}};
        QJsonArray clips;
        for (auto mode : {"STEP", "LINEAR"})
            clips.append(QJsonObject{
                {"name", mode},
                {"samplers", QJsonArray{QJsonObject{{"input", 0}, {"output", 1}, {"interpolation", mode}}}},
                {"channels",
                 QJsonArray{QJsonObject{{"sampler", 0},
                                        {"target", QJsonObject{{"node", 0}, {"path", "translation"}}}}}}});
        root["animations"] = clips;
        QTemporaryDir dir;
        QFile f(dir.filePath("native.gltf"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(root).toJson());
        f.close();
        Scene s;
        QString error;
        QVERIFY2(loadGltf(f.fileName(), s, error), qPrintable(error));
        QVERIFY(s.clips);
        QCOMPARE(s.clips->size(), size_t(2));
        QCOMPARE((*s.clips)[0].channels[0].times[1], .13f);
        s.evaluate(.129f * s.fps);
        QCOMPARE(s.pose[0].t.x(), 0.f);
        s.evaluate(.131f * s.fps);
        QCOMPARE(s.pose[0].t.x(), 1.f);
        QVERIFY(s.selectClip(1, error));
        s.evaluate(.065f * s.fps);
        QVERIFY(std::abs(s.pose[0].t.x() - .5f) < 1e-6f);
        auto json = sceneToJson(s);
        Scene restored;
        QVERIFY(sceneFromJson(json, restored, error));
        restored.evaluate(.065f * restored.fps);
        QVERIFY(std::abs(restored.pose[0].t.x() - .5f) < 1e-6f);
        restored.fps = 60;
        restored.last = 29;
        restored.evaluate(.065f * 60);
        QVERIFY(std::abs(restored.pose[0].t.x() - .5f) < 1e-6f);
        auto baked = restored.bakedCopy();
        QVERIFY(!baked.clips);
        QCOMPARE(baked.fps, 60);
        Window w;
        w.show();
        QVERIFY(w.open(f.fileName()));
        EditorApi api(&w);
        auto result = api.call("ui_set", {{"id", "motionClip"}, {"value", 1}});
        QVERIFY(!result["isError"].toBool());
        QCOMPARE(w.document().scene.sourceClip, 1);
        QCOMPARE(w.findChild<QTabBar *>("sceneTabs")->count(), 2);
        w.close();
    }
    void profileRenameHelpersAndMirrorAxes() {
        auto s = Scene::mannequin();
        s.profile = s.suggestedProfile();
        s.profile.configured = true;
        QCOMPARE(s.profile.chains.size(), 4);
        Node helper{"twist", 7, {s.nodes[8].rest.t * .5f, {}, {1, 1, 1}}, true};
        s.nodes.push_back(helper);
        s.nodes[8].parent = int(s.nodes.size()) - 1;
        s.nodes[8].rest.t *= .5f;
        s.pose = s.restPose();
        for (int n = 0; n < int(s.nodes.size()); ++n)
            s.nodes[n].name = QString("joint_%1").arg(n);
        QVERIFY(s.isEffector(8));
        QVERIFY(s.isFoot(15));
        QVERIFY(s.validateProfile());
        auto p = s.positions(s.pose);
        auto target = p[8] + QVector3D(-.15f, .1f, .1f);
        QVERIFY2(s.ikDiagnostic(8).isEmpty(), qPrintable(s.ikDiagnostic(8)));
        QVERIFY(s.solveIK(8, target, s.defaultPole(8)));
        auto after = s.positions(s.pose);
        QVERIFY((after[8] - target).length() < 1e-4f);
        QVERIFY(std::abs((after[8] - after[21]).length() - (p[8] - p[21]).length()) < 1e-5f);
        s.pose[5].s = {1, 2, 1};
        auto old = s.pose;
        QVERIFY(!s.solveIK(8, target, s.defaultPole(8)));
        comparePose(old, s.pose);
        QVERIFY(s.ikDiagnostic(8).contains("uniform"));
        auto rig = fittedPhysicsRig(s);
        for (auto b : rig)
            QVERIFY(b.node != 21);
        Scene mirror;
        mirror.nodes = {{"root", -1, {}, true},
                        {"a", 0, {{1, 0, 0}, QQuaternion::fromEulerAngles(35, 0, 0), {1, 1, 1}}, true},
                        {"b", 0, {{-1, 0, 0}, QQuaternion::fromEulerAngles(0, 50, 0), {1, 1, 1}}, true}};
        mirror.pose = mirror.restPose();
        mirror.profile.configured = true;
        mirror.profile.mirrors = {{1, 2}, {2, 1}};
        mirror.pose[1].r = mirror.pose[1].r * QQuaternion::fromEulerAngles(10, 20, 30);
        auto original = mirror.pose;
        auto delta =
            (mirror.globalRotation(1, original) * mirror.globalRotation(1, mirror.restPose()).conjugated())
                .normalized();
        mirror.mirrorPose();
        auto actual =
            (mirror.globalRotation(2, mirror.pose) * mirror.globalRotation(2, mirror.restPose()).conjugated())
                .normalized();
        QQuaternion expected(delta.scalar(), delta.x(), -delta.y(), -delta.z());
        QVERIFY(std::abs(QQuaternion::dotProduct(expected, actual)) > 1 - 1e-5f);
        mirror.mirrorPose();
        comparePose(original, mirror.pose);
    }
    void savedFingerMirrorPairs() {
        auto s = Scene::mannequin();
        s.nodes.push_back(
            {"LeftFinger", 8, {{.05f, 0, 0}, QQuaternion::fromEulerAngles(25, 0, 0), {1, 1, 1}}, true});
        s.nodes.push_back(
            {"RightFinger", 12, {{-.05f, 0, 0}, QQuaternion::fromEulerAngles(0, 35, 0), {1, 1, 1}}, true});
        s.pose = s.restPose();
        s.profile = s.suggestedProfile();
        s.profile.configured = true;
        QCOMPARE(s.profile.mirrors.value(21, -1), 22);
        for (int n = 0; n < int(s.nodes.size()); ++n)
            s.nodes[n].name = QString("joint_%1").arg(n);
        s.pose[21].r = s.pose[21].r * QQuaternion::fromEulerAngles(10, 20, 30);
        auto old = s.pose;
        auto delta = s.globalRotation(21, old) * s.globalRotation(21, s.restPose()).conjugated();
        s.mirrorPose();
        auto actual = s.globalRotation(22, s.pose) * s.globalRotation(22, s.restPose()).conjugated();
        QQuaternion expected(delta.scalar(), delta.x(), -delta.y(), -delta.z());
        QVERIFY(std::abs(QQuaternion::dotProduct(actual, expected)) > 1 - 1e-5f);
        s.mirrorPose();
        comparePose(s.pose, old);
    }
    void contactsHoldOrientationBetweenFrames() {
        auto s = Scene::mannequin();
        s.last = 40;
        s.profile = s.suggestedProfile();
        s.profile.configured = true;
        auto original = s.pose;
        s.keys[0] = {s.pose, {}, {}};
        s.pose[0].t += QVector3D(.025f, -.08f, .015f);
        s.pose[14].r = QQuaternion::fromEulerAngles(10, 0, 0);
        s.keys[40] = {s.pose, {}, {}};
        s.pose = original;
        auto p = s.positions(s.pose);
        Contact c{15, 0, 40, p[15], s.defaultPole(15)};
        c.orient = true;
        c.orientation = s.globalRotation(15, s.pose);
        c.fadeIn = 5;
        c.fadeOut = 5;
        s.contacts.push_back(c);
        for (float f : {5.f, 5.5f, 10.25f, 19.75f, 30.1f, 35.f}) {
            s.evaluate(f);
            QVERIFY((s.positions(s.pose)[15] - c.target).length() < .0002f);
            auto q = s.globalRotation(15, s.pose);
            QVERIFY(std::abs(QQuaternion::dotProduct(q, c.orientation)) > 1 - 1e-5f);
            auto result = s.pose;
            s.evaluate(1);
            s.evaluate(39);
            s.evaluate(f);
            comparePose(s.pose, result);
        }
        QCOMPARE(contactWeight(c, 0), 0.f);
        QVERIFY(std::abs(contactWeight(c, 2.5f) - .5f) < 1e-6f);
        QString error;
        Scene loaded;
        QVERIFY(sceneFromJson(sceneToJson(s), loaded, error));
        QVERIFY(loaded.contacts[0].orient);
        QCOMPARE(loaded.contacts[0].fadeIn, 5);
        s.jointLimits[15] = {true, {1, 0, 0}, 0, 0, 0};
        s.contacts[0].orientation = QQuaternion::fromAxisAndAngle(0, 1, 0, 35);
        auto conflict = s.diagnoseContacts();
        QVERIFY(conflict.angle > 5);
        QVERIFY(!conflict.warnings.empty());
    }
    void physicsFollowAndCancel() {
        Scene s;
        s.name = "Two bodies";
        s.last = 30;
        s.physics.gravity = 0;
        s.physics.ground = -100;
        s.nodes = {{"root", -1, {{0, 3, 0}, {}, {1, 1, 1}}, true},
                   {"child", 0, {{0, .5f, 0}, {}, {1, 1, 1}}, true}};
        s.pose = s.restPose();
        s.bodies = {{0, 2, .06f, {0, .2f, 0}, {0, .5f, 0}}, {1, 1, .04f, {0, .15f, 0}, {0, .3f, 0}}};
        s.keys[0] = {s.pose, {}, {}};
        s.keys[10] = s.keys[0];
        s.pose[1].r = QQuaternion::fromAxisAndAngle(0, 0, 1, 45);
        s.keys[20] = {s.pose, {}, {}};
        s.keys[30] = s.keys[20];
        s.evaluate(0);
        Scene passive, follow;
        PhysicsReport a, b;
        QString error;
        QVERIFY2(simulatePhysics(s, 0, 30, passive, a, error), qPrintable(error));
        s.physics.followAnimation = true;
        s.physics.stiffness = 200;
        s.physics.damping = 15;
        QVERIFY2(simulatePhysics(s, 0, 30, follow, b, error), qPrintable(error));
        passive.evaluate(30);
        follow.evaluate(30);
        float dp = std::abs(QQuaternion::dotProduct(passive.pose[1].r, s.keys[30].pose[1].r));
        float df = std::abs(QQuaternion::dotProduct(follow.pose[1].r, s.keys[30].pose[1].r));
        qInfo() << "Follow final quaternion alignment" << df << "passive" << dp;
        QVERIFY(df > dp + .02f);
        QVERIFY(b.following);
        auto limitedSource = s;
        limitedSource.jointLimits[1] = {true, {0, 0, 1}, 0, 0, 20};
        Scene limited;
        PhysicsReport limitReport;
        QVERIFY2(simulatePhysics(limitedSource, 0, 30, limited, limitReport, error), qPrintable(error));
        limited.evaluate(30);
        auto angle = limited.pose[1].r.toEulerAngles();
        QVERIFY(std::abs(angle.z()) <= 20.5f);
        QVERIFY(angle.z() > 15);
        // Internal torques cannot add net linear momentum in a free-flight interval.
        auto initial = physicalCenterOfMass(s), final = physicalCenterOfMass(follow);
        QVERIFY((initial - final).length() < .005f);
        PhysicsControl control;
        control.cancel = true;
        Scene untouched = Scene::mannequin();
        auto name = untouched.name;
        QVERIFY(!simulatePhysics(s, 0, 30, untouched, b, error, &control));
        QCOMPARE(untouched.name, name);
        Window w;
        w.show();
        s.last = 3000;
        s.keys[3000] = s.keys[30];
        w.document().replace(s);
        auto physicsDock = w.findChild<QDockWidget *>("physicsDock");
        physicsDock->show();
        physicsDock->raise();
        auto followControl = w.findChild<QCheckBox *>("physicsFollowAnimation");
        QTest::mouseClick(followControl, Qt::LeftButton, Qt::NoModifier,
                          QPoint(8, followControl->height() / 2));
        QVERIFY(!w.document().scene.physics.followAnimation);
        QTest::mouseClick(followControl, Qt::LeftButton, Qt::NoModifier,
                          QPoint(8, followControl->height() / 2));
        QVERIFY(w.document().scene.physics.followAnimation);
        EditorApi api(&w);
        auto result = api.call("physics_preview", {{"operation", "simulate"}});
        QVERIFY(!result["isError"].toBool());
        auto index = w.document().undo.index();
        auto source = w.document().scene.pose;
        auto state = api.call("editor_state", {});
        QVERIFY(!state["isError"].toBool());
        auto cancel = w.findChild<QPushButton *>("cancelPhysicsButton");
        qobject_cast<QScrollArea *>(physicsDock->widget())->ensureWidgetVisible(cancel);
        QTest::mouseClick(cancel, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(w.findChild<QLabel *>("physicsStats")->text().contains("Cancelled"), 30000);
        QCOMPARE(w.document().undo.index(), index);
        comparePose(w.document().scene.pose, source);
        api.call("physics_preview", {{"operation", "simulate"}});
        w.document().edit("Change target", [](Scene &scene) { scene.physics.gravity = 1; }, false);
        QTRY_VERIFY_WITH_TIMEOUT(w.findChild<QLabel *>("physicsStats")->text().contains("discarded"), 30000);
        QVERIFY(!w.viewport->physicsPreview);
        w.document().undo.setClean();
        w.close();
    }
    void pendingInputCannotApplyStalePhysics() {
        Window w;
        w.show();
        w.document().replace(Scene::mannequin());
        EditorApi api(&w);
        auto start = api.call("physics_preview", {{"operation", "simulate"}, {"first", 0}, {"last", 3}});
        QVERIFY(!start["isError"].toBool());
        auto apply = w.findChild<QPushButton *>("applyPhysicsButton");
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), 30000);
        QMetaObject::invokeMethod(w.findChild<QDoubleSpinBox *>("physicsStiffness"), "editingFinished",
                                  Qt::DirectConnection);
        QVERIFY(!w.document().dirty());
        QVERIFY(apply->isEnabled());
        auto box = w.findChild<QDoubleSpinBox *>("positionX");
        auto inspector = w.findChild<QDockWidget *>("inspectorDock");
        inspector->show();
        qobject_cast<QScrollArea *>(inspector->widget())->ensureWidgetVisible(box);
        box->setFocus();
        QTest::keyClick(box, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(box, QLocale().toString(.125, 'f', 3));
        QVERIFY(std::abs(w.document().scene.pose[0].t.x()) < 1e-6f);
        auto tabs = w.findChild<QTabBar *>("sceneTabs");
        int count = tabs->count();
        apply->click();
        QCOMPARE(tabs->count(), count);
        QVERIFY(!w.viewport->physicsPreview);
        QVERIFY(std::abs(w.document().scene.pose[0].t.x() - .125f) < 1e-5f);
        w.document().undo.setClean();
        w.close();
    }
    void keyedPinReleaseAllowsFlight() {
        Scene s;
        s.last = 15;
        s.physics.ground = -100;
        s.nodes = {{"root", -1, {{0, 2, 0}, {}, {1, 1, 1}}, true}};
        s.pose = s.restPose();
        s.bodies = {{0, 1, .05f, {}, {}}};
        s.keys[0] = {s.pose, {{0, Pin{{0, 2, 0}, {}, 1}}}, {}};
        s.keys[5] = {s.pose, {}, {}};
        s.evaluate(0);
        Scene heldSource = s;
        heldSource.keys[5] = heldSource.keys[0];
        Scene held, released;
        PhysicsReport report;
        QString error;
        QVERIFY2(simulatePhysics(heldSource, 0, 15, held, report, error), qPrintable(error));
        held.evaluate(15);
        QVERIFY(std::abs(held.pose[0].t.y() - 2) < .005f);
        QVERIFY2(simulatePhysics(s, 0, 15, released, report, error), qPrintable(error));
        released.evaluate(15);
        QVERIFY(released.pose[0].t.y() < 1.8f);
        s.physics.followAnimation = true;
        Scene following;
        QVERIFY2(simulatePhysics(s, 0, 15, following, report, error), qPrintable(error));
        following.evaluate(15);
        QVERIFY((following.pose[0].t - released.pose[0].t).length() < .001f);
    }
    void directionalLimitsAndLayerPins() {
        auto s = Scene::mannequin();
        s.jointLimits[7] = {true, {0, 0, 1}, 0, -20, 60};
        s.pose[7].r = QQuaternion::fromEulerAngles(25, 30, 110);
        s.enforceLimits();
        auto q = s.pose[7].r;
        QVector3D axis;
        float angle;
        q.getAxisAndAngle(&axis, &angle);
        QVERIFY(std::abs(axis.z()) > .999f);
        QVERIFY(angle <= 60.01f);
        s.jointLimits[7] = {false, {0, 0, 1}, 25, -10, 10};
        s.pose[7].r = QQuaternion::fromEulerAngles(50, 0, 40);
        s.enforceLimits();
        auto delta = s.pose[7].r;
        auto projected = QVector3D(0, 0, delta.z());
        QQuaternion twist(delta.scalar(), projected);
        twist.normalize();
        auto swing = delta * twist.conjugated();
        swing.getAxisAndAngle(&axis, &angle);
        QVERIFY(std::abs(angle) <= 25.01f);
        twist.getAxisAndAngle(&axis, &angle);
        QVERIFY(std::min(angle, 360 - angle) <= 10.01f);
        QString error;
        Scene restored;
        QVERIFY(sceneFromJson(sceneToJson(s), restored, error));
        QCOMPARE(restored.jointLimits.size(), 1);
        s = denseMotion();
        Document d;
        d.replace(s);
        d.seek(30);
        QVERIFY(d.scene.beginCorrection(0, 60, 10, 10, false, error));
        d.select(15);
        d.togglePin();
        QVERIFY(d.scene.correction().pins.contains(15));
        QVERIFY(d.scene.pins.contains(15));
        d.seek(31);
        QVERIFY(d.scene.pins.contains(15));
        d.scene.correction().enabled = false;
        d.scene.evaluate(31);
        QVERIFY(!d.scene.pins.contains(15));
        d.scene.correction().enabled = true;
        d.scene.evaluate(31);
        QVERIFY(d.scene.pins.contains(15));
        QVERIFY(sceneFromJson(sceneToJson(d.scene), restored, error));
        QCOMPARE(restored.correction().pins.size(), 1);
        s.contacts.push_back({15, 0, 60, {100, 0, 0}, s.defaultPole(15)});
        auto report = s.diagnoseContacts();
        QVERIFY(report.samples > 60);
        QVERIFY(report.position > 10);
        QVERIFY(!report.warnings.empty());
    }
    void demonstrationProject() {
        auto s = denseMotion();
        s.name = "Motion correction demo";
        s.profile = s.suggestedProfile();
        s.profile.configured = true;
        QString error;
        QVERIFY(s.beginCorrection(10, 50, 10, 10, false, error));
        s.evaluate(30);
        auto old = s.pose;
        auto p = s.positions(s.pose);
        QVERIFY(s.solveIK(8, p[8] + QVector3D(-.12f, .08f, .1f), s.defaultPole(8)));
        QVERIFY(s.captureCorrection(old, s.morphs, error));
        QVERIFY(saveProject("examples/motion-correction.trickster", s, error));
        QVERIFY(exportGltf("examples/motion-correction.glb", s, error));
        Window w;
        w.show();
        w.document().replace(s);
        w.document().seek(30);
        w.document().select(8);
        auto dock = w.findChild<QDockWidget *>("motionDock");
        dock->show();
        dock->raise();
        w.viewport->fit();
        QTest::qWait(100);
        QVERIFY(captureEditorWidget(&w).save("artifacts/motion-synthetic.png"));
        if (qEnvironmentVariableIsSet("TRICKSTER_UPDATE_DOCS"))
            QVERIFY(captureEditorWidget(&w).save("docs/motion-workspace.png"));
        w.document().undo.setClean();
        w.close();
    }
    void resamplePreservesTrimAndContactFades() {
        auto s = denseMotion();
        for (auto &key : s.keys)
            key.pose[0].t.setY(key.pose[0].t.y() - .1f);
        AnimationClip clip;
        clip.name = "Source";
        clip.duration = 2;
        clip.sampleFps = 30;
        clip.samples = s.keys;
        s.clips = std::make_shared<const std::vector<AnimationClip>>(std::vector<AnimationClip>{clip});
        s.sourceClip = 0;
        s.first = 10;
        s.last = 50;
        auto p = s.positions(s.restPose());
        Contact contact{15, 10, 50, p[15], s.defaultPole(15)};
        contact.orient = true;
        contact.fadeIn = 10;
        contact.fadeOut = 10;
        s.contacts.push_back(contact);
        s.evaluate(10);
        Window w;
        w.show();
        w.document().replace(s);
        w.findChild<QSpinBox *>("motionBakeFps")->setValue(60);
        w.findChild<QPushButton *>("bakeCorrectionButton")->click();
        QCOMPARE(w.document().scene.fps, 60);
        QCOMPARE(w.document().scene.first, 20);
        QCOMPARE(w.document().scene.last, 100);
        QVERIFY(!w.document().scene.clips);
        QVERIFY(w.document().scene.contacts.empty());
        for (int frame : {10, 15, 20, 30, 40, 45, 50}) {
            s.evaluate(float(frame));
            w.document().scene.evaluate(float(frame * 2));
            comparePose(s.pose, w.document().scene.pose);
        }
        w.document().undo.setClean();
        w.close();
    }
    void userMannequinWorkflow() {
        QString path = qEnvironmentVariable("TRICKSTER_TEST_CHARACTER");
        if (path.isEmpty())
            QSKIP("Optional user mannequin: set TRICKSTER_TEST_CHARACTER to run this additional workflow.");
        QVERIFY2(QFile::exists(path), qPrintable("Required fixture missing: " + path));
        Window w;
        w.show();
        QVERIFY(w.open(path));
        QString error;
        auto profile = w.document().scene.suggestedProfile();
        QCOMPARE(profile.chains.size(), 4);
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("characterProfileDialog");
            QVERIFY(dialog);
            auto pairs = dialog->findChild<QTreeWidget *>("profileMirrorPairs");
            QVERIFY(pairs);
            int count = pairs->topLevelItemCount();
            dialog->findChild<QPushButton *>("addMirrorPairButton")->click();
            QCOMPARE(pairs->topLevelItemCount(), count + 1);
            pairs->setCurrentItem(pairs->topLevelItem(count));
            dialog->findChild<QPushButton *>("removeMirrorPairButton")->click();
            QCOMPARE(pairs->topLevelItemCount(), count);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        w.findChild<QPushButton *>("characterProfileButton")->click();
        QVERIFY(w.document().scene.profile.configured);
        QVERIFY(w.document().scene.profile.mirrors.size() >= 40);
        int hand = profile.roles.value("leftHand", -1);
        QVERIFY(hand >= 0);
        w.document().select(hand);
        int end = w.document().scene.last;
        QVERIFY(end > 1);
        auto motion = w.findChild<QDockWidget *>("motionDock");
        motion->show();
        motion->raise();
        w.findChild<QSpinBox *>("correctionFadeIn")->setValue(std::min(5, end / 4));
        w.findChild<QSpinBox *>("correctionFadeOut")->setValue(std::min(5, end / 4));
        w.findChild<QPushButton *>("createCorrectionButton")->click();
        QVERIFY(w.document().scene.correction().present);
        auto initial = w.document().scene.pose;
        auto source = w.document().scene;
        auto positions = w.document().scene.positions(initial);
        int root = profile.chains[hand].root;
        auto target = positions[hand] + (positions[root] - positions[hand]) * .08f + QVector3D(0, .04f, 0);
        w.document().edit("Lift wrist", [&](Scene &s) {
            QVERIFY2(s.solveIK(hand, target, s.defaultPole(hand)), qPrintable(s.ikDiagnostic(hand)));
        });
        QVERIFY((w.document().scene.positions(w.document().scene.pose)[hand] - target).length() < .001f);
        auto toggle = w.findChild<QCheckBox *>("correctionEnabled");
        toggle->click();
        comparePose(initial, w.document().scene.pose);
        toggle->click();
        w.document().play(true);
        QTest::qWait(100);
        QVERIFY(w.document().playing);
        w.document().play(false);
        QDir().mkpath("artifacts/motion-demo");
        QVERIFY(saveProject("artifacts/motion-demo/HumanM-correction.trickster", w.document().scene, error));
        QVERIFY2(exportGltf("artifacts/motion-demo/HumanM-correction.glb", w.document().scene, error),
                 qPrintable(error));
        QVERIFY2(exportFbx("artifacts/motion-demo/HumanM-correction.fbx", w.document().scene, error),
                 qPrintable(error));
        Scene loaded;
        QVERIFY(loadGltf("artifacts/motion-demo/HumanM-correction.glb", loaded, error));
        QJsonArray reference;
        for (int frame : {0, end / 4, end / 2, end * 3 / 4, end}) {
            w.document().scene.evaluate(float(frame));
            loaded.evaluate(float(frame));
            auto expected = w.document().scene.skinned(w.document().scene.meshes[0]),
                 actual = loaded.skinned(loaded.meshes[0]);
            QCOMPARE(expected.size(), actual.size());
            for (size_t n = 0; n < expected.size(); ++n)
                QVERIFY((expected[n] - actual[n]).length() < .001f);
            QJsonObject joints;
            QJsonArray geometry;
            auto world = w.document().scene.positions(w.document().scene.pose);
            for (int n = 0; n < int(w.document().scene.nodes.size()); ++n)
                joints[w.document().scene.nodes[n].name] = QJsonArray{world[n].x(), world[n].y(), world[n].z()};
            for (const auto &mesh : w.document().scene.meshes)
                for (auto v : w.document().scene.skinned(mesh))
                    geometry.append(QJsonArray{v.x(), v.y(), v.z()});
            reference.append(QJsonObject{
                {"seconds", double(frame) / w.document().scene.fps}, {"joints", joints}, {"vertices", geometry}});
        }
        QFile referenceFile("artifacts/motion-demo/expected.json");
        QVERIFY(referenceFile.open(QIODevice::WriteOnly));
        referenceFile.write(QJsonDocument(reference).toJson(QJsonDocument::Compact));
        referenceFile.close();
        w.document().scene.evaluate(float(w.document().frame));
        w.viewport->fit();
        QTest::qWait(100);
        QVERIFY(captureEditorWidget(&w).save("artifacts/motion-workflow.png"));
        w.document().undo.setClean();
        w.close();
    }
    void renderBenchmark() {
        Window w;
        w.show();
        auto scene = denseMotion();
        QString error;
        if (QFile::exists("library/TestCharacter/TestCharacter_Rest.glb"))
            QVERIFY2(loadGltf("library/TestCharacter/TestCharacter_Rest.glb", scene, error),
                     qPrintable(error));
        w.document().replace(scene);
        w.viewport->fit();
        QTest::qWait(100);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 40; ++i) {
            auto camera = w.viewport->camera();
            camera.yaw += .1f;
            w.viewport->setCamera(camera);
            auto image = w.viewport->grabFramebuffer();
            QVERIFY(!image.isNull());
        }
        qInfo() << "40 orbit redraws (ms):" << timer.elapsed();
        w.close();
    }
    void controlsAndMcp() {
        Window w;
        w.show();
        w.document().replace(denseMotion());
        auto dock = w.findChild<QDockWidget *>("motionDock");
        dock->show();
        dock->raise();
        w.findChild<QSpinBox *>("correctionFrom")->setValue(10);
        w.findChild<QSpinBox *>("correctionTo")->setValue(50);
        w.findChild<QPushButton *>("createCorrectionButton")->click();
        QVERIFY(w.document().scene.correction().present);
        QCOMPARE(w.document().frame, 30);
        w.document().undo.setClean();
        QMetaObject::invokeMethod(w.findChild<QDoubleSpinBox *>("correctionStrength"), "editingFinished",
                                  Qt::DirectConnection);
        QVERIFY(!w.document().dirty());
        EditorApi api(&w);
        auto result = api.call("pose_transform", {{"node", 8}, {"position", QJsonArray{.28, .1, 0}}});
        QVERIFY2(!result["isError"].toBool(), qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
        QVERIFY(!w.document().scene.sampleLayer(w.document().scene.correction(),float(w.document().frame)).offsets.empty());
        w.document().select(7);
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QDialog *>("jointLimitDialog");
            QVERIFY(dialog);
            dialog->findChild<QComboBox *>("jointLimitKind")->setCurrentIndex(1);
            dialog->findChild<QDoubleSpinBox *>("jointLimitAxis0")->setValue(0);
            dialog->findChild<QDoubleSpinBox *>("jointLimitAxis2")->setValue(1);
            dialog->findChild<QDoubleSpinBox *>("jointLimitMin")->setValue(-30);
            dialog->findChild<QDoubleSpinBox *>("jointLimitMax")->setValue(60);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        });
        w.findChild<QPushButton *>("jointLimitButton")->click();
        QVERIFY(w.document().scene.jointLimits.contains(7));
        w.findChild<QToolButton *>("removeCorrectionButton")->click();
        QVERIFY(!w.document().scene.correction().present);
        w.document().undo.undo();
        QVERIFY(w.document().scene.correction().present);
        w.findChild<QCheckBox *>("correctionEnabled")->click();
        QVERIFY(!w.document().scene.correction().enabled);
        w.document().undo.undo();
        QVERIFY(w.document().scene.correction().enabled);
        auto session = w.document().captureSession();
        w.document().replace(Scene{});
        w.document().restoreSession(session);
        QVERIFY(w.document().scene.correction().present);
        QVERIFY(w.document().undo.canUndo());
        auto bad = api.call("motion_correction", {{"operation", "set"}, {"fadeIn", 10000}});
        QVERIFY(bad["isError"].toBool());
        QCOMPARE(w.document().scene.correction().fadeIn, 5);
        w.findChild<QPushButton *>("bakeCorrectionButton")->click();
        QVERIFY(!w.document().scene.correction().present);
        QCOMPARE(w.document().scene.keys.size(), 61);
    }
};
#include "atom_test_main.h"
int main(int argc,char** argv) { return runJesterTests<MotionTests>(argc,argv,"JesterMotionTests"); }
#include "motion_tests.moc"
