#include "io/project/project_io.h"
#include "settings/appearance.h"
#include "core/scene/edit_service.h"
#include "automation/editor_api.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QtTest>
using namespace trickster;
namespace {
Transform offset(float angle = 0) {
    Transform t;
    t.s = {0, 0, 0};
    t.r = QQuaternion::fromAxisAndAngle(0, 0, 1, angle);
    t.t = {.02f, 0, 0};
    return t;
}
Scene imported(bool layer = true) {
    Scene s = Scene::mannequin();
    s.first = 0;
    s.last = 30;
    s.fps = 30;
    s.profile = s.suggestedProfile();
    s.profile.configured = true;
    AnimationClip clip;
    clip.name = "Imported walk";
    clip.duration = 1;
    AnimationChannel channel;
    channel.node = 8;
    channel.path = "translation";
    channel.times = {0, 1.f / 3, 1};
    auto t = s.nodes[8].rest.t;
    for (int i = 0; i < 3; i++)
        for (float v : {t.x(), t.y(), t.z()})
            channel.values.push_back(v);
    clip.channels.push_back(channel);
    s.clips = std::make_shared<std::vector<AnimationClip>>(std::vector<AnimationClip>{clip});
    s.sourceClip = 0;
    s.evaluate(0);
    if (layer) {
        QString e;
        if (!s.beginCorrection(0, 30, 0, 0, false, e))
            qFatal("Layer creation failed");
        s.correction().keys.clear();
        for (int n : {6, 7, 8, 12})
            s.correction().keys[10].offsets[n] = offset(10 + n);
        s.correction().keys[10].morphOffsets[0] = std::vector<float>(s.nodes[0].morphRest.size(), .2f);
        s.correction().keys[0].offsets[8] = offset(0);
        s.correction().keys[30].offsets[8] = offset(60);
        s.evaluate(0);
    }
    return s;
}
void drag(Timeline &timeline, int from, int to, int y) {
    QTest::mousePress(&timeline, Qt::LeftButton, Qt::NoModifier,
                      QPoint(qRound(timeline.framePosition(from)), y));
    QTest::mouseMove(&timeline, QPoint(qRound(timeline.framePosition(to)), y), 25);
    QTest::mouseRelease(&timeline, Qt::LeftButton, Qt::NoModifier,
                        QPoint(qRound(timeline.framePosition(to)), y));
}
void clean(Window &w) {
    for (auto d : w.workspace.documents)
        d->undo.setClean();
    w.close();
}
} // namespace
class ScopeTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QSettings().clear();
        QDir().mkpath("artifacts/ux-scope");
    }
    void insertShortcutButtonAndNeutralUseExactChannel() {
        Window w;
        w.document().replace(imported());
        w.document().select(8);
        w.resize(1200, 850);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto &d = w.document();
        d.seek(15);
        auto original = sceneToJson(d.scene);
        QVERIFY(w.timeline->selectTrack(8, 2));
        QVERIFY(w.timeline->insertionScope().endsWith("Rotation"));
        w.activateWindow();
        w.raise();
        w.timeline->setFocus();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        QTest::keyClick(w.timeline, Qt::Key_I);
        auto key = d.scene.correction().keys.value(15);
        QCOMPARE(key.offsets.size(), 1);
        QVERIFY(key.offsets.contains(8));
        QCOMPARE(key.channels.value(8), 2);
        QVERIFY(key.morphOffsets.isEmpty());
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), original);
        QVERIFY(w.timeline->selectTrack(8, 2));
        w.findChild<QPushButton *>("insertKeyButton")->click();
        QCOMPARE(d.scene.correction().keys.value(15).channels.value(8), 2);
        QCOMPARE(d.scene.correction().keys.value(15).offsets.size(), 1);
        d.undo.undo();
        d.seek(10);
        auto old = d.scene.correction().keys[10];
        QVERIFY(w.timeline->selectTrack(8, 2));
        w.timeline->insertKey(true);
        auto neutral = d.scene.correction().keys[10];
        QCOMPARE(neutral.offsets[8].t, old.offsets[8].t);
        QCOMPARE(neutral.offsets[8].s, old.offsets[8].s);
        QCOMPARE(neutral.offsets[8].r, QQuaternion());
        QCOMPARE(neutral.morphOffsets, old.morphOffsets);
        QCOMPARE(neutral.offsets[12].r, old.offsets[12].r);
        QVERIFY(w.timeline->selectTrack(8));
        w.timeline->findChild<QScrollBar *>("timelineTrackScroll")->setValue(22);
        int parentRotation = w.timeline->trackCenter(6, 2);
        QVERIFY(parentRotation >= 64 && parentRotation < w.timeline->height() - 22);
        QTest::mouseClick(w.timeline, Qt::LeftButton, Qt::NoModifier, QPoint(25, parentRotation));
        QCOMPARE(d.selection(), QSet<int>({6}));
        QVERIFY(w.timeline->insertionScope().contains(d.scene.nodes[6].name));
        QVERIFY(w.timeline->insertionScope().endsWith("Rotation"));
        clean(w);
        Document raw;
        raw.replace(Scene::mannequin());
        raw.select(8);
        auto source = raw.scene.keys;
        Timeline timeline(&raw);
        timeline.resize(900, 150);
        timeline.show();
        QVERIFY(QTest::qWaitForWindowExposed(&timeline));
        QVERIFY(timeline.selectTrack(8, 2, true));
        timeline.insertKey();
        QVERIFY(raw.scene.correction().present);
        QCOMPARE(raw.scene.correction().keys[0].channels.value(8), 2);
        QCOMPARE(raw.scene.correction().keys[0].offsets.size(), 1);
        QVERIFY(raw.scene.keys.keys() == source.keys());
        QVERIFY(timeline.insertionScope().endsWith("Rotation"));
        QVERIFY(timeline.trackCenter(8, 2) >= 64 && timeline.trackCenter(8, 2) < timeline.height() - 22);
        raw.seek(10);
        timeline.insertKey();
        QCOMPARE(raw.scene.correction().keys[10].channels.value(8), 2);
    }
    void symmetryPreservesUneditedOppositeComponents() {
        auto s = Scene::mannequin();
        s.profile = s.suggestedProfile();
        s.profile.configured = true;
        s.pose[10].t += QVector3D(.03f, .02f, .01f);
        s.pose[10].s = {1.1f, 1.1f, 1.1f};
        s.pose[10].r = QQuaternion::fromAxisAndAngle(0, 1, 0, 12) * s.pose[10].r;
        s.keys[0] = {s.pose, s.pins, s.morphs};
        Document d;
        d.replace(s);
        d.select(6);
        d.setSymmetry(true);
        auto original = sceneToJson(d.scene);
        auto old = d.scene.pose[10];
        EditService::execute(d, "pose_transform", {{"node", 6}, {"rotation", QJsonArray{0, 0, 20}}});
        QCOMPARE(d.scene.pose[10].t, old.t);
        QCOMPARE(d.scene.pose[10].s, old.s);
        QVERIFY(std::abs(QQuaternion::dotProduct(d.scene.pose[10].r, old.r)) < .999f);
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), original);
        auto source = d.scene.pose[6].t + QVector3D(.01f, .01f, 0);
        EditService::execute(d, "pose_transform",
                             {{"node", 6}, {"position", QJsonArray{source.x(), source.y(), source.z()}}});
        QCOMPARE(d.scene.pose[10].r, old.r);
        QCOMPARE(d.scene.pose[10].s, old.s);
        QVERIFY(d.scene.pose[10].t != old.t);
        d.undo.undo();
        EditService::execute(d, "pose_transform", {{"node", 6}, {"scale", QJsonArray{1.05, 1.05, 1.05}}});
        QCOMPARE(d.scene.pose[10].t, old.t);
        QCOMPARE(d.scene.pose[10].r, old.r);
        QVERIFY(d.scene.pose[10].s != old.s);
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), original);
    }
    void adjacentKeyGroupMovesAtomicallyAndRejectsCollisions() {
        Document d;
        d.replace(imported());
        auto &c = d.scene.correction();
        c.keys[11].offsets[8] = offset(35);
        c.keys[11].channels[8] = 2;
        auto original = sceneToJson(d.scene);
        QJsonArray keys{QJsonObject{{"frame", 10}, {"node", 8}, {"channels", 2}},
                        QJsonObject{{"frame", 11}, {"node", 8}, {"channels", 2}}};
        EditService::execute(d, "motion_correction",
                             {{"operation", "keys_transform"}, {"keys", keys}, {"delta", 1}});
        QCOMPARE(d.scene.correction().keys[11].offsets[8].r, offset(18).r);
        QCOMPARE(d.scene.correction().keys[12].offsets[8].r, offset(35).r);
        QCOMPARE(d.scene.correction().keys[10].channels.value(8), 5);
        QVERIFY(d.scene.correction().keys[10].offsets.contains(12));
        QVERIFY(d.scene.correction().keys[10].morphOffsets.contains(0));
        QCOMPARE(d.undo.index(), 1);
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), original);
        QVERIFY_EXCEPTION_THROWN(
            EditService::execute(
                d, "motion_correction",
                {{"operation", "keys_transform"}, {"keys", keys}, {"delta", 1}, {"copy", true}}),
            std::runtime_error);
        QCOMPARE(sceneToJson(d.scene), original);
        QCOMPARE(d.undo.index(), 0);
        auto count = d.undo.count();
        QVERIFY_EXCEPTION_THROWN(
            EditService::execute(d, "motion_correction",
                                 {{"operation", "keys_transform"}, {"keys", keys}, {"delta", 30}}),
            std::runtime_error);
        QCOMPARE(sceneToJson(d.scene), original);
        QCOMPARE(d.undo.count(), count);
    }
    void timelineGroupSelectionCopyPasteAndDelete() {
        Document d;
        d.replace(imported());
        d.select(8);
        d.scene.correction().keys[20].offsets[8] = offset(25);
        d.scene.correction().keys[20].channels[8] = 2;
        Timeline timeline(&d);
        timeline.resize(1100, 500);
        timeline.show();
        QVERIFY(QTest::qWaitForWindowExposed(&timeline));
        QVERIFY(timeline.selectTrack(8, 2));
        int y = timeline.trackCenter(8, 2);
        auto click = [&](int frame, Qt::KeyboardModifiers modifiers) {
            QTest::mouseClick(&timeline, Qt::LeftButton, modifiers,
                              QPoint(qRound(timeline.framePosition(frame)), y));
        };
        click(10, Qt::NoModifier);
        click(20, Qt::ShiftModifier);
        QCOMPARE(timeline.selectedKeyCount(), 2);
        drag(timeline, 10, 11, y);
        QCOMPARE(d.scene.correction().keys[11].channels.value(8), 2);
        QCOMPARE(d.scene.correction().keys[21].channels.value(8), 2);
        QCOMPARE(d.undo.index(), 1);
        QTest::keyClick(&timeline, Qt::Key_C, Qt::ControlModifier);
        d.seek(12);
        QTest::keyClick(&timeline, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(d.scene.correction().keys[12].channels.value(8), 2);
        QCOMPARE(d.scene.correction().keys[22].channels.value(8), 2);
        QCOMPARE(d.undo.index(), 2);
        d.undo.undo();
        QVERIFY(!d.scene.correction().keys.contains(12));
        click(11, Qt::NoModifier);
        click(21, Qt::ShiftModifier);
        QTest::keyClick(&timeline, Qt::Key_Delete);
        QVERIFY(!d.scene.correction().keys.contains(11));
        QVERIFY(!d.scene.correction().keys.contains(21));
        d.undo.undo();
        QVERIFY(d.scene.correction().keys.contains(11));
        QVERIFY(d.scene.correction().keys.contains(21));
        // Shift-drag selects editable diamonds while leaving source rows immutable.
        QVERIFY(timeline.selectTrack(8, 2));
        y = timeline.trackCenter(8, 2);
        QPoint a(qRound(timeline.framePosition(11)) - 12, y - 8),
            b(qRound(timeline.framePosition(21)) + 12, y + 8);
        QTest::mousePress(&timeline, Qt::LeftButton, Qt::ShiftModifier, a);
        QTest::mouseMove(&timeline, b, 20);
        QTest::mouseRelease(&timeline, Qt::LeftButton, Qt::ShiftModifier, b);
        QCOMPARE(timeline.selectedKeyCount(), 2);
    }
    void multipleControllerGestureAndSelectionState() {
        Window w;
        w.document().replace(Scene::mannequin());
        w.document().select(-1);
        w.resize(1100, 850);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.viewport->fit();
        QTest::qWait(50);
        auto &d = w.document();
        auto initial = d.scene.positions(d.scene.pose);
        for (int n : {15, 19})
            QTest::mouseClick(w.viewport, Qt::LeftButton, Qt::ShiftModifier,
                              w.viewport->project(initial[n]).toPoint());
        QCOMPARE(d.selection(), QSet<int>({15, 19}));
        EditorApi api(&w);
        QCOMPARE(api.state(false)["selection"].toArray().size(), 2);
        auto before = sceneToJson(d.scene);
        auto start = w.viewport->project(initial[15]).toPoint();
        QTest::mousePress(w.viewport, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(w.viewport, start + QPoint(0, -12), 30);
        QTest::mouseRelease(w.viewport, Qt::LeftButton, Qt::NoModifier, start + QPoint(0, -12));
        auto after = d.scene.positions(d.scene.pose);
        for (int n : {15, 19})
            QVERIFY((after[n] - initial[n]).length() > .005f);
        QCOMPARE(d.undo.index(), 1);
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), before);
        QCOMPARE(d.selection().size(), 2);
        d.togglePin();
        QVERIFY(d.scene.pins.contains(15));
        QVERIFY(d.scene.pins.contains(19));
        QCOMPARE(d.undo.index(), 1);
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), before);
        auto saved = d.captureSession();
        Document other;
        other.restoreSession(saved);
        QCOMPARE(other.selection(), d.selection());
        auto result = api.call("node_select", {{"nodes", QJsonArray{8, 12}}});
        QVERIFY(!result["isError"].toBool());
        QCOMPARE(d.selection(), QSet<int>({8, 12}));
        clean(w);
    }
    void inspectorFollowsGizmoSpace() {
        Window w;
        auto s = Scene::mannequin();
        s.pose[0].r = QQuaternion::fromAxisAndAngle(0, 1, 0, 30);
        s.keys[0] = {s.pose, s.pins, s.morphs};
        w.document().replace(s);
        w.document().select(8);
        auto &d = w.document();
        auto p = d.scene.positions(d.scene.pose)[8];
        QCOMPARE(w.findChild<QDoubleSpinBox *>("positionX")->value(), double(qRound(p.x() * 1000) / 1000.));
        auto target = d.scene.globalRotation(8, d.scene.pose).toEulerAngles() + QVector3D(0, 10, 0);
        EditService::execute(d, "pose_transform",
                             {{"node", 8},
                              {"rotation", QJsonArray{target.x(), target.y(), target.z()}},
                              {"rotationSpace", "world"}});
        auto expected = QQuaternion::fromEulerAngles(target);
        QVERIFY(std::abs(QQuaternion::dotProduct(d.scene.globalRotation(8, d.scene.pose), expected)) >
                1 - 1e-5f);
        auto action = w.findChild<QAction *>("localAction");
        if (!action)
            for (auto a : w.findChildren<QAction *>())
                if (a->text() == "Local axes")
                    action = a;
        QVERIFY(action);
        action->trigger();
        QVERIFY(w.viewport->local);
        QCOMPARE(w.findChild<QDoubleSpinBox *>("positionX")->value(),
                 double(qRound(d.scene.pose[8].t.x() * 1000) / 1000.));
        clean(w);
    }
    void channelKeysPreserveOtherTracksAndReload() {
        Document d;
        d.replace(imported());
        auto original = sceneToJson(d.scene);
        auto native = d.scene.clips;
        auto oldTranslation = d.scene.sampleLayer(d.scene.correction(), 7.5f).offsets[8].t;
        EditService::execute(
            d, "motion_correction",
            {{"operation", "key_move"}, {"from", 10}, {"frame", 20}, {"node", 8}, {"channels", 2}});
        auto &c = d.scene.correction();
        QVERIFY(c.keys[10].offsets.contains(12));
        QVERIFY(c.keys[10].morphOffsets.contains(0));
        QCOMPARE(c.keys[10].channels.value(8), 5);
        QCOMPARE(c.keys[20].channels.value(8), 2);
        QCOMPARE(d.scene.sampleLayer(c, 7.5f).offsets[8].t, oldTranslation);
        QCOMPARE(d.scene.sampleLayer(c, 15).offsets[8].t, oldTranslation);
        auto expected = QQuaternion::fromAxisAndAngle(0, 0, 1, 13.5f);
        QVERIFY(std::abs(QQuaternion::dotProduct(d.scene.sampleLayer(c, 15).offsets[8].r, expected)) >
                1 - 1e-6f);
        QCOMPARE(d.scene.clips, native);
        QTemporaryDir tmp;
        QString error;
        QVERIFY2(saveProject(tmp.path() + "/channels.jst", d.scene, error), qPrintable(error));
        Scene loaded;
        QVERIFY2(loadProject(tmp.path() + "/channels.jst", loaded, error), qPrintable(error));
        QCOMPARE(sceneToJson(loaded)["version"].toInt(), 8);
        for (float f : {0.f, 7.5f, 10.f, 15.f, 20.f, 30.f}) {
            auto a = d.scene, b = loaded;
            a.evaluate(f);
            b.evaluate(f);
            for (size_t n = 0; n < a.pose.size(); n++) {
                QVERIFY((a.pose[n].t - b.pose[n].t).length() < 1e-6);
                QVERIFY(std::abs(QQuaternion::dotProduct(a.pose[n].r, b.pose[n].r)) > 1 - 1e-6);
            }
        }
        QCOMPARE(d.undo.count(), 1);
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), original);
        auto before = sceneToJson(d.scene);
        d.scene.correction().keys[20].offsets[7] = offset();
        before = sceneToJson(d.scene);
        int count = d.undo.count();
        bool failed = false;
        try {
            EditService::execute(
                d, "motion_correction",
                {{"operation", "key_move"}, {"from", 10}, {"frame", 20}, {"nodes", QJsonArray{6, 7, 8}}});
        } catch (...) {
            failed = true;
        }
        QVERIFY(failed);
        QCOMPARE(sceneToJson(d.scene), before);
        QCOMPARE(d.undo.count(), count);
    }
    void immutableSourceCannotCaptureCorrectionKey() {
        Document d;
        d.replace(imported());
        d.select(8);
        Timeline timeline(&d);
        timeline.resize(900, 420);
        timeline.show();
        QVERIFY(QTest::qWaitForWindowExposed(&timeline));
        auto before = sceneToJson(d.scene);
        drag(timeline, 10, 15, timeline.summaryTrackCenter(false));
        d.seek(0);
        QCOMPARE(sceneToJson(d.scene), before);
        QTest::keyClick(&timeline, Qt::Key_Delete);
        QCOMPARE(sceneToJson(d.scene), before);
        int y = timeline.trackCenter(8);
        QVERIFY(y > timeline.summaryTrackCenter(true));
        drag(timeline, 10, 20, y);
        auto &keys = d.scene.correction().keys;
        for (int n : {6, 7, 8}) {
            QVERIFY(keys[20].offsets.contains(n));
            QVERIFY(!keys[10].offsets.contains(n));
        }
        QVERIFY(keys[10].offsets.contains(12));
        QVERIFY(keys[10].morphOffsets.contains(0));
        QCOMPARE(d.undo.count(), 1);
        d.undo.undo();
        d.seek(0);
        QCOMPARE(sceneToJson(d.scene), before);
        QTest::mouseClick(&timeline, Qt::LeftButton, Qt::NoModifier,
                          QPoint(qRound(timeline.framePosition(10)), y));
        QCOMPARE(timeline.selectedKey, 10);
        d.select(12);
        QCOMPARE(timeline.selectedKey, -1);
        auto beforeDelete = sceneToJson(d.scene);
        QTest::keyClick(&timeline, Qt::Key_Delete);
        QCOMPARE(sceneToJson(d.scene), beforeDelete);
        d.select(8);
        // Expand the controller and independently retime only its end bone's rotation.
        QTest::mouseClick(&timeline, Qt::LeftButton, Qt::NoModifier, QPoint(8, y));
        QCoreApplication::processEvents();
        int rotation = timeline.trackCenter(8, 2);
        QVERIFY(rotation > y);
        drag(timeline, 10, 15, rotation);
        QCOMPARE(d.scene.correction().keys[10].channels.value(8), 5);
        QCOMPARE(d.scene.correction().keys[15].channels.value(8), 2);
        QVERIFY(d.scene.correction().keys[10].offsets.contains(6));
        QVERIFY(d.scene.correction().keys[10].offsets.contains(12));
    }
    void symmetricIKAndFKHaveOneUndo() {
        Document d;
        auto scene = Scene::mannequin();
        scene.profile = scene.suggestedProfile();
        scene.profile.configured = true;
        d.replace(scene);
        d.select(8);
        d.setSymmetry(true);
        QCOMPARE(d.scene.mirrorPartner(8), 12);
        auto initial = sceneToJson(d.scene);
        auto positions = d.scene.positions(d.scene.pose);
        auto target = positions[8] + QVector3D(-.02f, .12f, .04f);
        EditService::execute(d, "pose_ik",
                             {{"node", 8}, {"target", QJsonArray{target.x(), target.y(), target.z()}}});
        auto p = d.scene.positions(d.scene.pose);
        QVERIFY(std::abs(p[8].x() + p[12].x()) < .001f);
        QVERIFY(std::abs(p[8].y() - p[12].y()) < .001f);
        QVERIFY(std::abs(p[8].z() - p[12].z()) < .001f);
        QCOMPARE(d.undo.count(), 1);
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), initial);
        d.select(6);
        d.begin();
        auto old = d.scene.pose;
        d.scene.rotate(6, {0, 0, 1}, 15, true);
        d.scene.mirrorChanged(6, old);
        QVERIFY(std::abs(QQuaternion::dotProduct(d.scene.pose[10].r, old[10].r)) < .999f);
        d.commit("Symmetric shoulder");
        QCOMPARE(d.undo.index(), 1);
        d.undo.undo();
        QCOMPARE(sceneToJson(d.scene), initial);
        d.scene.profile.mirrors.clear();
        QCOMPARE(d.scene.mirrorPartner(8), -1);
        auto previous = d.scene.pose[8];
        d.scene.mirrorPose();
        QCOMPARE(d.scene.pose[8].t, previous.t);
        QCOMPARE(d.scene.pose[8].r, previous.r);
    }
    void symmetryWithMovingRootAndDifferentRestAxes() {
        auto s = Scene::mannequin();
        s.profile = s.suggestedProfile();
        s.profile.configured = true;
        auto basis = QQuaternion::fromAxisAndAngle(0, 0, 1, 90);
        s.nodes[10].rest.r = basis * s.nodes[10].rest.r;
        s.nodes[11].rest.t = basis.conjugated().rotatedVector(s.nodes[11].rest.t);
        s.nodes[11].rest.r = basis.conjugated() * s.nodes[11].rest.r;
        s.pose = s.restPose();
        s.pose[0].r = QQuaternion::fromAxisAndAngle(0, 1, 0, 25);
        s.pose[0].t.setX(.3f);
        auto positions = s.positions(s.pose);
        s.pins[12] = {positions[12], s.defaultPole(12), 1};
        s.keys.clear();
        s.keys[0] = {s.pose, s.pins, {}};
        Document d;
        d.replace(s);
        d.select(8);
        d.setSymmetry(true);
        auto frame = s.globalRotation(0, s.pose);
        auto target = positions[8] + frame.rotatedVector({-.04f, .08f, .02f});
        EditService::execute(d, "pose_ik",
                             {{"node", 8}, {"target", QJsonArray{target.x(), target.y(), target.z()}}});
        auto after = d.scene.positions(d.scene.pose);
        auto origin = after[0];
        auto local = frame.conjugated().rotatedVector(after[8] - origin);
        local.setX(-local.x());
        auto reflected = origin + frame.rotatedVector(local);
        QVERIFY2((after[12] - reflected).length() < .002f,
                 qPrintable(QString::number((after[12] - reflected).length())));
        QVERIFY((d.scene.pins[12].target - reflected).length() < .002f);
    }
    void recordingLocksRejectEveryKeyEntryPoint() {
        Document d;
        for (int mode = 0; mode < 3; mode++) {
            d.replace(imported(mode != 2));
            if (mode == 0)
                d.scene.correction().editing = false;
            else
                d.scene.compareOriginal = true;
            auto old = sceneToJson(d.scene);
            for (auto method :
                 {QString("timeline"), QString("pose_transform"), QString("motion_correction")}) {
                if (mode == 2 && method == "motion_correction")
                    continue;
                QJsonObject args = method == "timeline" ? QJsonObject{{"operation", "insert"}}
                                   : method == "pose_transform"
                                       ? QJsonObject{{"node", 8}, {"rotation", QJsonArray{0, 15, 0}}}
                                       : QJsonObject{{"operation", "key_reset"}, {"node", 8}};
                QVERIFY_EXCEPTION_THROWN(EditService::execute(d, method, args), std::runtime_error);
                QCOMPARE(sceneToJson(d.scene), old);
                QCOMPARE(d.undo.index(), 0);
            }
            d.edit("Blocked gesture", [](Scene &s) { s.rotate(8, {0, 1, 0}, 15, true); });
            QCOMPARE(sceneToJson(d.scene), old);
            QCOMPARE(d.undo.index(), 0);
        }
    }
    void mcpUsesSymmetrySessionForSingleAndBatchEdits() {
        Window w;
        auto scene = imported(false);
        w.document().replace(scene);
        w.document().select(8);
        w.document().setSymmetry(true);
        EditorApi api(&w);
        QVERIFY(api.state(false)["symmetryEditing"].toBool());
        QCOMPARE(api.state()["nodes"].toArray()[8].toObject()["mirrorPartner"].toInt(), 12);
        auto old = sceneToJson(w.document().scene);
        auto positions = w.document().scene.positions(w.document().scene.pose);
        auto target = positions[8] + QVector3D(-.04f, .08f, .02f);
        QJsonObject args{
            {"node", 8}, {"target", QJsonArray{target.x(), target.y(), target.z()}}, {"pin", true}};
        for (bool batch : {false, true}) {
            auto result =
                batch
                    ? api.call("batch", {{"operations",
                                          QJsonArray{QJsonObject{{"tool", "pose_ik"}, {"arguments", args}}}}})
                    : api.call("pose_ik", args);
            QVERIFY2(!result["isError"].toBool(),
                     qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
            auto after = w.document().scene.positions(w.document().scene.pose);
            QVERIFY((after[12] - positions[12]).length() > .001f);
            QCOMPARE(w.document().undo.index(), 1);
            w.document().undo.undo();
            QCOMPARE(sceneToJson(w.document().scene), old);
        }
        clean(w);
    }
    void viewportSymmetryGestureUndo() {
        Window w;
        auto scene = Scene::mannequin();
        scene.profile = scene.suggestedProfile();
        scene.profile.configured = true;
        w.document().replace(scene);
        w.document().select(8);
        w.document().setSymmetry(true);
        double scale = w.devicePixelRatioF();
        w.resize(qRound(1280 / scale), qRound(720 / scale));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.viewport->fit();
        QTest::qWait(60);
        auto before = sceneToJson(w.document().scene);
        auto initial = w.document().scene.positions(w.document().scene.pose);
        auto point = w.viewport->project(initial[8]).toPoint();
        QTest::mousePress(w.viewport, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseMove(w.viewport, point + QPoint(-8, -8), 30);
        auto preview = w.document().scene.positions(w.document().scene.pose);
        QVERIFY((preview[12] - initial[12]).length() > .001f);
        QTest::mouseRelease(w.viewport, Qt::LeftButton, Qt::NoModifier, point + QPoint(-8, -8));
        QCOMPARE(w.document().undo.count(), 1);
        QVERIFY(captureEditorWidget(&w).save("artifacts/ux-scope/symmetry-" + QString::number(scale) + ".png"));
        QTest::keyClick(w.viewport, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(sceneToJson(w.document().scene), before);
        clean(w);
    }
    void recordingPairSelectionAndCharacterCommands() {
        Window w;
        w.document().replace(imported(false));
        double scale = w.devicePixelRatioF();
        w.resize(qRound(1280 / scale), qRound(720 / scale));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.document().select(15);
        auto label = w.findChild<QLabel *>("mirrorPartnerLabel");
        QVERIFY(label);
        QVERIFY(label->text().contains(w.document().scene.nodes[19].name));
        w.document().select(-1);
        auto feet = w.findChild<QPushButton *>("pinFeetButton");
        QVERIFY(feet->isEnabled());
        feet->click();
        QVERIFY(w.document().scene.correction().present);
        QVERIFY(w.document().scene.pins.size() >= 2);
        w.document().undo.undo();
        w.document().select(8);
        w.document().seek(15);
        auto recording = w.findChild<QLabel *>("timelineRecordingContext");
        QVERIFY(recording->text().contains("First edit"));
        EditService::execute(w.document(), "pose_transform",
                             {{"node", 8}, {"rotation", QJsonArray{0, 15, 0}}});
        QVERIFY(recording->text().contains("Record:"));
        auto compare = w.findChild<QCheckBox *>("timelineCompare");
        compare->click();
        QVERIFY(w.document().scene.compareOriginal);
        QVERIFY(recording->text().contains("Recording OFF"));
        QVERIFY(!w.findChild<QPushButton *>("insertKeyButton")->isEnabled());
        compare->click();
        auto opposite = w.findChild<QPushButton *>("selectOppositeButton");
        opposite->click();
        QCOMPARE(w.document().selected, 12);
        auto beforeDialog = sceneToJson(w.document().scene);
        bool visited = false;
        QTimer::singleShot(80, &w, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            QTimer::singleShot(2000, dialog, &QDialog::reject);
            auto pairs = dialog->findChild<QTreeWidget *>("profileMirrorPairs");
            QVERIFY(pairs);
            QVERIFY(pairs->currentItem());
            bool selected = false;
            for (int col = 0; col < 2; col++)
                if (auto combo = qobject_cast<QComboBox *>(pairs->itemWidget(pairs->currentItem(), col)))
                    selected |= combo->currentData().toInt() == 12;
            QVERIFY(selected);
            visited = true;
            dialog->reject();
        });
        w.findChild<QPushButton *>("configureMirrorPairsButton")->click();
        QVERIFY(visited);
        QCOMPARE(sceneToJson(w.document().scene), beforeDialog);
        QVERIFY2(w.width() * scale <= 1282,
                 qPrintable(QString("Window width %1 at scale %2").arg(w.width()).arg(scale)));
        QVERIFY2(w.height() * scale <= 722,
                 qPrintable(QString("Window height %1 at scale %2").arg(w.height()).arg(scale)));
        QVERIFY2(w.timeline->mapTo(&w, QPoint(0, w.timeline->height())).y() <= w.height() - 18,
                 "Timeline extends below the client area");
        QVERIFY(w.timeline->visibleRegion().boundingRect().height() >= w.timeline->height() - 2);
        auto inspectorScroll = w.findChild<QScrollArea *>("inspectorScroll");
        QVERIFY(inspectorScroll);
        QTRY_COMPARE(inspectorScroll->horizontalScrollBar()->maximum(), 0);
        auto physicsScroll = w.findChild<QScrollArea *>("physicsScroll");
        QVERIFY(physicsScroll);
        auto physicsDock = w.findChild<QDockWidget *>("physicsDock");
        physicsDock->raise();
        QTest::qWait(30);
        QTRY_COMPARE(physicsScroll->horizontalScrollBar()->maximum(), 0);
        w.findChild<QDockWidget *>("inspectorDock")->raise();

        QVERIFY(captureEditorWidget(&w).save("artifacts/ux-scope/workspace-" + QString::number(scale) + ".png"));
        clean(w);
    }
};
#include "atom_test_main.h"
int main(int argc,char** argv) { return runJesterTests<ScopeTests>(argc,argv,"JesterUxScopeTests"); }
#include "ux_scope_tests.moc"
