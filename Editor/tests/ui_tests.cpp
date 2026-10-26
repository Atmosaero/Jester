#include "io/project/project_io.h"
#include "settings/appearance.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QBuffer>
#include <QJsonDocument>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QtTest>
#include <algorithm>
using namespace trickster;
class UITests : public QObject {
    Q_OBJECT
  private:
    bool focusViewport(Window &w) {
        // Window-scoped QAction shortcuts require an active top-level window.
        // Exposed native rendering surfaces alone do not guarantee activation.
        w.raise();
        w.activateWindow();
        if (!QTest::qWaitForWindowActive(&w))
            return false;
        w.viewport->setFocus(Qt::OtherFocusReason);
        return QTest::qWaitFor([&] { return w.viewport->hasFocus(); });
    }
    void acceptFile(QString path) {
        QTimer::singleShot(200, [path] {
            for (auto w : QApplication::topLevelWidgets())
                if (auto dialog = qobject_cast<QFileDialog *>(w)) {
                    dialog->setDirectory(QFileInfo(path).absolutePath());
                    dialog->selectFile(QFileInfo(path).fileName());
                    if (auto edit = dialog->findChild<QLineEdit *>("fileNameEdit"))
                        edit->setText(QFileInfo(path).fileName());
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                    return;
                }
        });
    }
    void drag(Viewport *view, QPoint from, QPoint to) {
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(view, to, 40);
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, to);
        QTest::qWait(50);
    }
  private slots:
    void initTestCase() {
        QDir().mkpath("artifacts");
        QSettings().clear();
    }
    void completeUserWorkflow() {
        QTemporaryDir temp;
        Window w;
        w.document().replace(Scene::mannequin());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(200);
        QVERIFY(w.viewport->isValid());
        auto feet = w.document().scene.positions(w.document().scene.pose);
        auto pin = w.findChild<QPushButton *>("pinFeetButton");
        QVERIFY(pin);
        QTest::mouseClick(pin, Qt::LeftButton);
        QCOMPARE(w.document().scene.pins.size(), 2);
        w.viewport->setFocus();
        w.viewport->viewPreset(1);
        QTest::qWait(50);
        QPoint hips = w.viewport->project(feet[0]).toPoint();
        drag(w.viewport, hips, hips + QPoint(0, 32));
        QCOMPARE(w.document().selected, 0);
        auto p = w.document().scene.positions(w.document().scene.pose);
        QVERIFY(p[0].y() < feet[0].y() - .03f);
        for (int n : {15, 19})
            QVERIFY((p[n] - feet[n]).length() < .001f);
        w.findChild<QSpinBox *>("currentFrame")->setValue(30);
        QPoint hand = w.viewport->project(w.document().scene.positions(w.document().scene.pose)[8]).toPoint();
        drag(w.viewport, hand, hand + QPoint(-36, -28));
        QCOMPARE(w.document().selected, 8);
        QVERIFY(w.document().scene.keys.contains(30));
        QVERIFY((w.document().scene.positions(w.document().scene.pose)[8] - p[8]).length() > .05f);
        auto tree = w.findChild<QTreeWidget *>("skeletonTree");
        QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toInt(), 8);
        auto targetY = w.findChild<QDoubleSpinBox *>("targetY");
        QVERIFY(std::abs(targetY->value() - w.document().scene.positions(w.document().scene.pose)[8].y()) <
                .001);
        w.findChild<QSpinBox *>("currentFrame")->setValue(60);
        w.document().select(4);
        QVERIFY(focusViewport(w));
        QTest::keyClick(w.viewport, Qt::Key_E);
        QCOMPARE(w.viewport->tool, Viewport::Rotate);
        auto head = w.viewport->project(w.document().scene.positions(w.document().scene.pose)[4]).toPoint();
        drag(w.viewport, head, head + QPoint(25, 0));
        QVERIFY(w.document().scene.keys.contains(60));
        QVERIFY(std::abs(w.document().scene.pose[4].r.scalar() - 1) > .0001f);
        QCOMPARE(w.document().scene.keys.size(), 3);
        QVERIFY(focusViewport(w));
        QTest::keyClick(w.viewport, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!w.document().scene.keys.contains(60));
        QVERIFY(focusViewport(w));
        QTest::keyClick(w.viewport, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(w.document().scene.keys.contains(60));
        w.document().seek(0);
        QVERIFY(focusViewport(w));
        QTest::keyClick(w.viewport, Qt::Key_Space);
        QVERIFY(w.document().playing);
        QTest::qWait(160);
        QVERIFY(w.document().frame > 0);
        QVERIFY(focusViewport(w));
        QTest::keyClick(w.viewport, Qt::Key_Space);
        QVERIFY(!w.document().playing);
        QString project = temp.path() + "/user.jst";
        acceptFile(project);
        w.findChild<QAction *>("saveAction")->trigger();
        QVERIFY2(QFileInfo::exists(project),
                 qPrintable("Expected " + project + "; saved " + w.document().path));
        QVERIFY(!w.document().dirty());
        Scene before = w.document().scene;
        w.document().replace(Scene::mannequin());
        QVERIFY(w.open(project));
        QCOMPARE(w.document().scene.keys.size(), 3);
        QString glb = temp.path() + "/user.glb";
        acceptFile(glb);
        w.findChild<QAction *>("exportAction")->trigger();
        QVERIFY(QFileInfo::exists(glb));
        QVERIFY(w.open(glb));
        QVERIFY(w.document().scene.keys.size() > 3);
        w.document().seek(30);
        before.evaluate(30);
        auto a = before.positions(before.pose), b = w.document().scene.positions(w.document().scene.pose);
        for (size_t i = 0; i < a.size(); ++i)
            QVERIFY((a[i] - b[i]).length() < .0002f);
        w.document().select(8);
        QTest::qWait(100);
        QVERIFY(captureEditorWidget(&w).save("artifacts/ui-workflow.png"));
        w.document().undo.setClean();
        w.close();
    }
    void cancelDragAndTimeline() {
        Window w;
        w.document().replace(Scene::mannequin());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(100);
        auto p = w.document().scene.positions(w.document().scene.pose);
        auto hand = w.viewport->project(p[8]).toPoint();
        QTest::mousePress(w.viewport, Qt::LeftButton, Qt::NoModifier, hand);
        QTest::mouseMove(w.viewport, hand + QPoint(-20, -20));
        QTest::keyClick(w.viewport, Qt::Key_Escape);
        QTest::mouseRelease(w.viewport, Qt::LeftButton, Qt::NoModifier, hand + QPoint(-20, -20));
        QVERIFY((w.document().scene.positions(w.document().scene.pose)[8] - p[8]).length() < .00001f);
        QVERIFY(w.document().scene.keys.isEmpty());
        w.document().insertKey();
        w.document().seek(30);
        w.document().insertKey();
        w.timeline->fit();
        QTest::qWait(50);
        QPoint from(int(w.timeline->framePosition(30)), w.timeline->summaryTrackCenter(false)),
            to(int(w.timeline -> framePosition(45)), w.timeline->summaryTrackCenter(false));
        QTest::mousePress(w.timeline, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(w.timeline, to);
        QTest::mouseRelease(w.timeline, Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY(w.document().scene.keys.contains(45));
        QVERIFY(!w.document().scene.keys.contains(30));
        QTest::keyClick(w.timeline, Qt::Key_C, Qt::ControlModifier);
        w.document().seek(60);
        QTest::keyClick(w.timeline, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(w.document().scene.keys.contains(60));
        QTest::keyClick(w.timeline, Qt::Key_Delete);
        QVERIFY(!w.document().scene.keys.contains(60));
        w.document().undo.setClean();
        w.close();
    }

    void helperControlsAndExternalRender() {
        Window w;
        w.document().replace(Scene::mannequin());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(100);
        w.document().select(4);
        auto enabled = w.findChild<QCheckBox *>("limitEnabled");
        QVERIFY(enabled);
        QTest::mouseClick(enabled, Qt::LeftButton);
        QVERIFY(w.document().scene.rotationLimits.contains(4));
        auto assist = w.findChild<QAction *>("bodyAssistAction");
        assist->trigger();
        QVERIFY(w.document().scene.bodyAssist);
        w.document().select(15);
        bool added = false;
        QTimer contactDriver;
        contactDriver.setInterval(25);
        connect(&contactDriver,&QTimer::timeout,&w,[&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName()!="contactDialog" || !dialog->isVisible())
                return;
            contactDriver.stop();
            if (auto button = dialog->findChild<QPushButton *>("contactAddButton"))
                QTest::mouseClick(button, Qt::LeftButton);
            auto list = dialog->findChild<QListWidget *>();
            added = list && list->count() == 1;
            dialog->reject();
        });
        contactDriver.start();
        w.findChild<QAction *>("contactsAction")->trigger();
        QVERIFY(added);
        QCOMPARE(w.document().scene.contacts.size(), 1);
        w.findChild<QAction *>("balanceAction")->trigger();
        QVERIFY(w.viewport->showBalance);
        QTest::qWait(80);
        QVERIFY(captureEditorWidget(&w).save("artifacts/ui-helpers.png"));
        QString path = "artifacts/external/CesiumMan.glb";
        if (QFileInfo::exists(path)) {
            Scene s;
            QString error;
            QVERIFY2(loadGltf(path, s, error), qPrintable(error));
            w.document().replace(std::move(s));
            w.document().seek(15);
            QTest::qWait(200);
            QVERIFY(captureEditorWidget(&w).save("artifacts/ui-external.png"));
        }
        w.document().undo.setClean();
        w.close();
    }

    void correctionCopies() {
        QTemporaryDir temp;
        Window w;
        w.document().replace(Scene::mannequin());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.document().demo();
        auto originalKeys = w.document().scene.keys.size();
        QString baked = temp.path() + "/baked.jst";
        acceptFile(baked);
        auto bakeAction = w.findChild<QAction *>("bakeCopyAction");
        QVERIFY(bakeAction);
        bakeAction->trigger();
        QVERIFY(QFileInfo::exists(baked));
        Scene b;
        QString error;
        QVERIFY(loadProject(baked, b, error));
        QCOMPARE(b.keys.size(), 91);
        QVERIFY(b.pins.isEmpty());
        QCOMPARE(w.document().scene.keys.size(), originalKeys);
        QString arc = temp.path() + "/arc.jst";
        bool dialogSeen = false;
        QTimer::singleShot(150, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            dialogSeen = dialog->windowTitle().contains("Experimental COM arc");
            dialog->grab().save("artifacts/ui-arc-dialog.png");
            acceptFile(arc);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        auto arcAction = w.findChild<QAction *>("arcCopyAction");
        QVERIFY(arcAction);
        arcAction->trigger();
        QVERIFY(dialogSeen);
        QVERIFY(QFileInfo::exists(arc));
        Scene jump;
        QVERIFY(loadProject(arc, jump, error));
        QVERIFY(jump.name.contains("experimental"));
        QCOMPARE(w.document().scene.keys.size(), originalKeys);
        w.document().undo.setClean();
        w.close();
    }

    void emptyDefaultAndSceneTabs() {
        Window w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(80);
        auto tabs = w.findChild<QTabBar *>("sceneTabs");
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(w.document().scene.nodes.empty());
        QVERIFY(w.findChild<QWidget *>("emptyScenePanel")->isVisible());
        QVERIFY(!w.findChild<QAction *>("exportAction")->isEnabled());
        QVERIFY(focusViewport(w));
        QTest::keyClick(w.viewport, Qt::Key_I);
        QVERIFY(w.document().scene.keys.isEmpty());
        QVERIFY(captureEditorWidget(&w).save("artifacts/empty-scene.png"));
        QTest::mouseClick(w.findChild<QPushButton *>("emptyMannequinButton"), Qt::LeftButton);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(!w.document().scene.nodes.empty());
        w.document().edit("Move first scene", [](Scene &s) { s.translateWorld(0, {.2f, 0, 0}); });
        w.viewport->viewPreset(2);
        auto camera = w.viewport->camera();
        QVERIFY(focusViewport(w));
        QTest::keyClick(w.viewport, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(tabs->count(), 2);
        QVERIFY(w.document().scene.nodes.empty());
        QVERIFY(!w.document().dirty());
        w.switchScene(0);
        QVERIFY(w.document().dirty());
        QCOMPARE(w.viewport->camera().yaw, camera.yaw);
        QVERIFY(std::abs(w.document().scene.pose[0].t.x() - .2f) < .0001f);
        w.document().undo.undo();
        QVERIFY(!w.document().dirty());
        QVERIFY(std::abs(w.document().scene.pose[0].t.x()) < .0001f);
        w.switchScene(1);
        QVERIFY(!w.document().undo.canUndo());
        w.switchScene(0);
        QVERIFY(w.document().undo.canRedo());
        w.document().undo.redo();
        QVERIFY(std::abs(w.document().scene.pose[0].t.x() - .2f) < .0001f);
        w.document().undo.setClean();
        w.closeScene(0);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(w.document().scene.nodes.empty());
        w.closeScene(0);
        QCOMPARE(tabs->count(), 1);
        w.document().undo.setClean();
        w.close();
    }
    void resourceCatalogIsNotASceneLibrary() {
        QTemporaryDir temp;
        QDir root(temp.path());
        QVERIFY(root.mkpath("resources/nested"));
        QString error;
        const auto hidden = root.filePath("resources/character.jst");
        const auto nested = root.filePath("resources/nested/animation.jst");
        const auto visible = root.filePath("visible.jst");
        QTemporaryDir unmounted;
        const auto recentResource = unmounted.filePath("recent.jst");
        QFile externalCatalog(unmounted.filePath("catalog.json"));
        QVERIFY(externalCatalog.open(QIODevice::WriteOnly));
        externalCatalog.write(R"({"resourceOnly":true})");
        externalCatalog.close();
        for (const auto &path : {hidden, nested, visible, recentResource})
            QVERIFY2(saveProject(path, Scene::mannequin(), error), qPrintable(error));
        QFile catalog(root.filePath("resources/catalog.json"));
        QVERIFY(catalog.open(QIODevice::WriteOnly));
        catalog.write(R"({"resourceOnly":true,"entries":[{"file":"character.jst"}]})");
        catalog.close();
        QSettings settings;
        auto folder = settings.value("sceneLibraryFolder"), recent = settings.value("recentScenes");
        const auto restore = qScopeGuard([&] {
            settings.setValue("sceneLibraryFolder", folder);
            settings.setValue("recentScenes", recent);
        });
        settings.setValue("sceneLibraryFolder", temp.path());
        settings.setValue("recentScenes", QStringList{hidden, nested, visible, recentResource});
        int visibleCount = 0, mannequinCount = 0;
        for (const auto &choice : sceneChoices()) {
            QVERIFY(choice.path != hidden);
            QVERIFY(choice.path != nested);
            QVERIFY(choice.path != recentResource);
            visibleCount += choice.path == visible;
            mannequinCount += choice.kind == SceneChoice::Mannequin;
        }
        QCOMPARE(visibleCount, 1);
        QCOMPARE(mannequinCount, 1);
        // A resource is still a normal file, fully usable through Open File.
        Scene preserved;
        QVERIFY2(loadProject(hidden, preserved, error), qPrintable(error));
        QVERIFY(!preserved.nodes.empty());
        QVERIFY(QFileInfo::exists(nested));
    }
    void chooseEpicScenes() {
        const auto choices = sceneChoices();
        for (auto name : {QString("Manny"), QString("Quinn")}) {
            auto choice = std::find_if(choices.begin(), choices.end(), [&](const auto &c) {
                return c.name == name && c.category == "Characters";
            });
            if (choice == choices.end())
                QSKIP("Local Epic mannequin library is not installed");
            Window w;
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            bool selected = false, unanimated = false;
            QTimer::singleShot(200, [&] {
                auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto search = dialog->findChild<QLineEdit *>("sceneSearch");
                auto list = dialog->findChild<QListWidget *>("sceneChoiceList");
                search->setText(name);
                if (!list->count()) {
                    dialog->reject();
                    return;
                }
                list->setCurrentRow(0);
                selected = true;
                unanimated = !dialog->findChild<QPushButton *>("previewAnimationButton")->isEnabled();
                dialog->grab().save("artifacts/epic-picker-" + name + ".png");
                QTest::mouseClick(dialog->findChild<QPushButton *>("openSceneButton"), Qt::LeftButton);
            });
            w.findChild<QAction *>("chooseSceneAction")->trigger();
            QVERIFY(selected);
            QVERIFY(unanimated);
            auto &d = w.document();
            auto &scene = d.scene;
            QVERIFY(scene.nodes.size() > 80);
            QVERIFY(!scene.meshes.empty());
            QVERIFY(scene.keys.isEmpty());
            QVERIFY(!scene.clips || scene.clips->empty());
            QVERIFY(scene.layers.empty());
            QVERIFY(scene.pins.isEmpty());
            size_t vertices = 0, textures = 0;
            for (const auto &mesh : scene.meshes) {
                vertices += mesh.vertices.size();
                textures += mesh.textures.size();
            }
            QVERIFY(vertices > 10000);
            QVERIFY(textures > 0);
            auto profile = scene.suggestedProfile();
            QCOMPARE(profile.chains.size(), 4);
            int hand = profile.roles.value("leftHand", -1);
            QVERIFY(hand >= 0);
            auto before = scene.positions(scene.pose)[hand];
            QVERIFY2(scene.ikDiagnostic(hand).isEmpty(), qPrintable(scene.ikDiagnostic(hand)));
            d.edit("Check Epic rig", [&](Scene &s) {
                QVERIFY(s.solveIK(hand, before + QVector3D(0, .06f, .04f), s.defaultPole(hand)));
            });
            QVERIFY((scene.positions(scene.pose)[hand] - before).length() > .01f);
            d.undo.undo();
            QVERIFY((scene.positions(scene.pose)[hand] - before).length() < 1e-5f);
            QVERIFY(scene.keys.isEmpty());
            d.select(-1);
            QTest::qWait(150);
            QVERIFY(captureEditorWidget(&w).save("artifacts/epic-scene-" + name + ".png"));
            auto tabs = w.findChild<QTabBar *>("sceneTabs");
            QCOMPARE(tabs->count(), 1);
            w.newEmptyScene();
            QCOMPARE(tabs->count(), 2);
            QVERIFY(w.document().scene.keys.isEmpty());
            w.switchScene(0);
            QVERIFY(w.document().scene.keys.isEmpty());
            w.document().undo.setClean();
            w.close();
        }
    }

    void closeUnsavedSceneGuard() {
        Window w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateSceneChoice({SceneChoice::Mannequin, "", "", "", ""});
        w.document().edit("Pose", [](Scene &s) { s.translateWorld(0, {.2f, 0, 0}); });
        w.newEmptyScene();
        auto tabs = w.findChild<QTabBar *>("sceneTabs");
        QCOMPARE(tabs->count(), 2);
        QTimer::singleShot(150, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box)
                QTest::mouseClick(box->button(QMessageBox::Cancel), Qt::LeftButton);
        });
        w.closeScene(0);
        QCOMPARE(tabs->count(), 2);
        QVERIFY(w.document().scene.nodes.empty());
        w.switchScene(0);
        QVERIFY(w.document().dirty());
        QVERIFY(std::abs(w.document().scene.pose[0].t.x() - .2f) < .0001f);
        QTimer::singleShot(150, [] {
            auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (box)
                QTest::mouseClick(box->button(QMessageBox::Discard), Qt::LeftButton);
        });
        w.closeScene(0);
        QCOMPARE(tabs->count(), 1);
        QVERIFY(w.document().scene.nodes.empty());
        w.close();
    }

    void pendingInspectorAcrossTabs() {
        Window w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateSceneChoice({SceneChoice::Mannequin, "", "", "", ""});
        auto box = w.findChild<QDoubleSpinBox *>("positionX");
        QVERIFY(box);
        box->setFocus();
        QTest::keyClick(box, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(box, QLocale().toString(.37, 'f', 3));
        QVERIFY(std::abs(w.document().scene.pose[0].t.x()) < .0001f);
        w.newEmptyScene();
        QVERIFY(w.document().scene.nodes.empty());
        w.switchScene(0);
        QVERIFY2(std::abs(w.document().scene.pose[0].t.x() - .37f) < .0001f,
                 "Uncommitted numeric edit was lost on scene switch");
        QVERIFY(w.document().dirty());
        QVERIFY(w.document().scene.keys.contains(0));
        w.document().undo.undo();
        QVERIFY(std::abs(w.document().scene.pose[0].t.x()) < .0001f);
        w.document().undo.setClean();
        box->setFocus();
        QTest::keyClick(box, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(box, QLocale().toString(.41, 'f', 3));
        QTimer::singleShot(150, [] {
            auto message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (message)
                QTest::mouseClick(message->button(QMessageBox::Cancel), Qt::LeftButton);
        });
        w.closeScene(0);
        QCOMPARE(w.findChild<QTabBar *>("sceneTabs")->count(), 2);
        QVERIFY(std::abs(w.document().scene.pose[0].t.x() - .41f) < .0001f);
        QVERIFY(w.document().dirty());
        QTemporaryDir temp;
        QString file = temp.path() + "/pending.jst";
        QString error;
        QVERIFY(saveProject(file, w.document().scene, error));
        w.document().path = file;
        w.document().undo.setClean();
        box->setFocus();
        QTest::keyClick(box, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(box, QLocale().toString(.44, 'f', 3));
        w.findChild<QAction *>("saveAction")->trigger();
        Scene saved;
        QVERIFY(loadProject(file, saved, error));
        QVERIFY(std::abs(saved.pose[0].t.x() - .44f) < .0001f);
        QVERIFY(!w.document().dirty());
        w.close();
    }
    void texturesMorphsAndPhysicsPreview() {
        Scene s;
        // Compare authored texels in the neutral display mode. Lit intentionally
        // includes the project's manual exposure and tonemapper.
        QString visualError;QVERIFY(s.visuals.setValue("mode","unlit",visualError));
        s.nodes = {{"Morph", -1, {}, true, {0}}};
        s.pose = s.restPose();
        s.last = 30;
        Mesh m;
        m.node = 0;
        m.color = Qt::white;
        m.unlit = true;
        for (auto p : std::vector<QVector3D>{{-.5f, 0, 0}, {.5f, 0, 0}, {.5f, 1, 0}, {-.5f, 1, 0}}) {
            Vertex v;
            v.p = p;
            v.normal = {0, 0, 1};
            v.uv = {p.x() + .5f, 1 - p.y()};
            m.vertices.push_back(v);
        }
        m.indices = {0, 1, 2, 0, 2, 3};
        m.targets.push_back({"Stretch", {{0, 0, 0}, {0, 0, 0}, {0, .4f, 0}, {0, .4f, 0}}, {}, {}});
        QImage checker(2, 2, QImage::Format_RGBA8888);
        checker.setPixelColor(0, 0, Qt::red);
        checker.setPixelColor(1, 0, Qt::green);
        checker.setPixelColor(0, 1, Qt::blue);
        checker.setPixelColor(1, 1, Qt::yellow);
        Texture texture;
        QBuffer buf(&texture.bytes);
        buf.open(QIODevice::WriteOnly);
        checker.save(&buf, "PNG");
        texture.minFilter = 9728;
        texture.magFilter = 9728;
        m.textures["baseColor"] = texture;
        s.meshes.push_back(m);
        s.morphs = s.restMorphs();
        s.keys[0] = {s.pose, {}, s.morphs};
        s.morphs[0] = {1};
        s.keys[30] = {s.pose, {}, s.morphs};
        s.evaluate(0);
        Window w;
        w.document().replace(s);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.viewport->showBones = false;
        w.viewport->viewPreset(1);
        QTest::qWait(120);
        auto pixel = w.viewport->project({-.25f, .75f, 0}).toPoint();
        auto image = w.viewport->grabFramebuffer();
        image.save("artifacts/unlit-texture-frame.png");
        QFile renderReport("artifacts/unlit-texture-render.json");
        if(renderReport.open(QIODevice::WriteOnly)) renderReport.write(QJsonDocument(w.viewport->renderingStats()).toJson());
        pixel *= w.viewport->devicePixelRatioF();
        auto color = image.pixelColor(pixel);
        QVERIFY2(color.red() > 200 && color.green() < 40 && color.blue() < 40, qPrintable(color.name()));
        auto sampleMaterial=[&](Scene variant,const QString& label) {
            w.document().replace(std::move(variant));
            w.viewport->viewPreset(1);
            auto frame=w.viewport->grabFramebuffer();
            frame.save("artifacts/material-"+label+".png");
            auto point=w.viewport->project({-.25f,.75f,0}).toPoint()*w.viewport->devicePixelRatioF();
            return frame.pixelColor(point);
        };
        auto transformed=s;
        transformed.meshes[0].textures["baseColor"].offset={.5f,0};
        color=sampleMaterial(transformed,"uv-offset");
        QVERIFY2(color.green()>150 && color.green()>color.red()+80 && color.green()>color.blue()+80,qPrintable(color.name()));
        transformed.meshes[0].textures["baseColor"].offset={1,0};
        transformed.meshes[0].textures["baseColor"].wrapS=33071;
        color=sampleMaterial(transformed,"clamp");
        const auto gpuClamp=color;
        w.viewport->forceCpuSkinning=true;const auto cpuClamp=sampleMaterial(transformed,"clamp-cpu");w.viewport->forceCpuSkinning=false;
        QCOMPARE(gpuClamp,cpuClamp);
        QVERIFY2(color.green()>150 && color.green()>color.red()+80 && color.green()>color.blue()+80,qPrintable(color.name()));
        transformed.meshes[0].textures["baseColor"].wrapS=10497;
        color=sampleMaterial(transformed,"repeat");
        QVERIFY2(color.red()>200 && color.green()<40,qPrintable(color.name()));
        auto secondUv=s;
        secondUv.meshes[0].textures["baseColor"].coord=1;
        for(auto& vertex:secondUv.meshes[0].vertices) vertex.uv1={1-vertex.uv.x(),vertex.uv.y()};
        color=sampleMaterial(secondUv,"uv1");
        const auto gpuUvColor=color;
        w.viewport->forceCpuSkinning=true;const auto cpuUvColor=sampleMaterial(secondUv,"uv1-cpu");w.viewport->forceCpuSkinning=false;
        QCOMPARE(gpuUvColor,cpuUvColor);
        QVERIFY2(color.green()>150 && color.green()>color.red()+80 && color.green()>color.blue()+80,qPrintable(color.name()));
        auto cutout=s;
        cutout.meshes[0].alphaMode="MASK";
        cutout.meshes[0].color.setAlphaF(0);
        color=sampleMaterial(cutout,"alpha-mask");
        QVERIFY2(std::abs(color.red()-color.green())<12 && std::abs(color.blue()-color.green())<12,qPrintable(color.name()));
        const auto background=color;
        auto blend=s;blend.meshes[0].alphaMode="BLEND";blend.meshes[0].color.setAlphaF(.5);
        color=sampleMaterial(blend,"alpha-blend");
        QVERIFY2(color.red()>background.red()+20 && color.red()<250,qPrintable(color.name()));
        const auto gpuBlend=color;w.viewport->forceCpuSkinning=true;const auto cpuBlend=sampleMaterial(blend,"alpha-blend-cpu");w.viewport->forceCpuSkinning=false;
        QCOMPARE(gpuBlend,cpuBlend);
        sampleMaterial(s,"restored");
        auto dock = w.findChild<QDockWidget *>("shapesDock");
        dock->show();
        dock->raise();
        w.document().seek(15);
        auto weight = w.findChild<QDoubleSpinBox *>("morphWeight_0_0");
        QVERIFY(weight);
        QCOMPARE(weight->value(), 50.);
        weight->setFocus();
        weight->setValue(75);
        QTest::keyClick(weight, Qt::Key_Return);
        QVERIFY(std::abs(w.document().scene.morphWeights(0)[0] - .75f) < .0001f);
        QVERIFY(w.document().scene.keys.contains(15));
        w.document().undo.undo();
        QVERIFY(std::abs(w.document().scene.morphWeights(0)[0] - .5f) < .0001f);
        w.document().undo.redo();
        QVERIFY(captureEditorWidget(&w).save("artifacts/ui-shapes.png"));
        QTemporaryDir exported;
        auto fbx = exported.path() + "/shapes.fbx";
        acceptFile(fbx);
        w.findChild<QAction *>("exportAction")->trigger();
        QVERIFY(QFile::exists(fbx));
        Scene reimported;
        QString mediaError;
        QVERIFY2(loadFbx(fbx, reimported, mediaError), qPrintable(mediaError));
        QCOMPARE(reimported.meshes[0].targets.size(), size_t(1));
        QCOMPARE(reimported.meshes[0].textures.size(), 1);

        w.document().replace(Scene::mannequin());
        auto physics = w.findChild<QDockWidget *>("physicsDock");
        physics->show();
        physics->raise();
        w.findChild<QSpinBox *>("physicsTo")->setValue(15);
        auto before = w.document().scene.pose;
        QTest::mouseClick(w.findChild<QPushButton *>("simulatePhysicsButton"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(w.findChild<QPushButton *>("applyPhysicsButton")->isEnabled(), 30000);
        QCOMPARE(w.document().scene.pose[0].t, before[0].t);
        QVERIFY(w.document().scene.keys.isEmpty());
        QTest::mouseClick(w.findChild<QPushButton *>("playButton"), Qt::LeftButton);
        QVERIFY(w.document().playing);
        QTRY_VERIFY_WITH_TIMEOUT(w.document().frame > 0,5000);
        QTest::mouseClick(w.findChild<QPushButton *>("playButton"), Qt::LeftButton);
        QVERIFY(!w.document().playing);
        w.document().seek(10);
        QTest::qWait(50);
        QVERIFY(captureEditorWidget(&w).save("artifacts/ui-physics.png"));
        QTest::mouseClick(w.findChild<QPushButton *>("applyPhysicsButton"), Qt::LeftButton);
        QVERIFY(w.document().scene.name.contains("dynamics"));
        QVERIFY(w.document().dirty());
        QVERIFY(w.document().scene.keys.size() > 2);
        w.document().undo.setClean();
        w.close();
    }
    void compactLayout() {
        Window w;
        w.document().replace(Scene::mannequin());
        w.resize(1000, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(100);
        QVERIFY(w.viewport->width() >= 320);
        QVERIFY(w.viewport->height() >= 240);
        auto button = w.findChild<QPushButton *>("insertKeyButton");
        QVERIFY(button->isVisible());
        QVERIFY(w.rect().contains(button->mapTo(&w, button->rect().bottomRight())));
        w.document().select(8);
        QTest::qWait(50);
        QVERIFY(captureEditorWidget(&w).save("artifacts/ui-compact.png"));
        w.document().undo.setClean();
        w.close();
    }
};
#include "atom_test_main.h"
int main(int argc,char** argv) { return runJesterTests<UITests>(argc,argv,"JesterUiTests"); }
#include "ui_tests.moc"
