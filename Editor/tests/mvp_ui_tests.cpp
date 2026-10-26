#include "ui/widgets/localized_ui.h"
#include "io/project/project_io.h"
#include "settings/appearance.h"
#include "io/catalog/demo_scenes.h"
#include "core/scene/edit_service.h"
#include "automation/editor_api.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QJsonDocument>
#include <QBuffer>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTableWidget>
#include <QtTest>
using namespace trickster;
class MVPUI : public QObject {
    Q_OBJECT
    void fileChoice(QString path) {
        QTimer::singleShot(60, [path] {
            for (auto widget : QApplication::topLevelWidgets())
                if (auto dialog = qobject_cast<QFileDialog *>(widget)) {
                    dialog->setOption(QFileDialog::DontConfirmOverwrite, true);
                    dialog->setDirectory(QFileInfo(path).absolutePath());
                    dialog->selectFile(QFileInfo(path).fileName());
                    if (auto line = dialog->findChild<QLineEdit *>("fileNameEdit"))
                        line->setText(QFileInfo(path).fileName());
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                    return;
                }
        });
    }
    void selectNode(Window &w, int node) {
        auto dock = w.findChild<QDockWidget *>("skeletonDock");
        dock->show();
        dock->raise();
        auto tree = w.findChild<QTreeWidget *>("skeletonTree");
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toInt() == node) {
                auto item = *it;
                for (auto p = item->parent(); p; p = p->parent())
                    p->setExpanded(true);
                tree->scrollToItem(item);
                QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                                  tree->visualItemRect(item).center());
                return;
            }
            ++it;
        }
    }
    void visible(Window &w, QWidget *widget) {
        for (auto p = widget->parentWidget(); p; p = p->parentWidget())
            if (auto dock = qobject_cast<QDockWidget *>(p)) {
                dock->show();
                dock->raise();
                break;
            }
        for (auto p = widget->parentWidget(); p; p = p->parentWidget())
            if (auto scroll = qobject_cast<QScrollArea *>(p)) {
                scroll->ensureWidgetVisible(widget);
                break;
            }
        QTest::qWait(20);
    }
  private slots:
    void motionToolsWorkflow() {
        Window w;Scene source=Scene::mannequin();source.keys[0]={source.pose,{},source.morphs};source.pose[6].r=QQuaternion::fromEulerAngles(0,0,35);source.keys[30]={source.pose,{},source.morphs};source.last=30;w.document().replace(source);auto original=sceneToJson(w.document().scene);w.workspace.append(Scene::mannequin(),{},false);w.resize(qRound(1920/w.devicePixelRatioF()),qRound(1080/w.devicePixelRatioF()));w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
        int stage=0;QTimer timer;QElapsedTimer elapsed;elapsed.start();connect(&timer,&QTimer::timeout,&w,[&]{auto dialog=qobject_cast<QDialog*>(activeEditorModalWidget());if(!dialog)return;if(auto error=qobject_cast<QMessageBox*>(dialog)){qWarning()<<error->text();error->reject();return;}if(elapsed.elapsed()>15000){dialog->reject();return;}for(auto button:dialog->findChildren<QPushButton*>()){if(stage==0&&button->text()=="Calculate preview"){stage=1;button->click();return;}if(stage==1&&button->text()=="Create result"&&button->isEnabled()){stage=2;dialog->grab().save(QString("artifacts/motion-retarget-%1.png").arg(w.devicePixelRatioF()));button->click();return;}}});timer.start(100);w.showRetarget();timer.stop();QCOMPARE(stage,2);QCOMPARE(w.workspace.documents.size(),size_t(3));QCOMPARE(sceneToJson(w.workspace.documents[0]->scene),original);
        w.document().select(8);
        stage=0;elapsed.restart();connect(&timer,&QTimer::timeout,&w,[&]{auto dialog=qobject_cast<QDialog*>(activeEditorModalWidget());if(!dialog)return;if(elapsed.elapsed()>10000){dialog->reject();return;}for(auto button:dialog->findChildren<QPushButton*>())if(stage==0&&button->text()=="Calculate / update paths"){stage=1;button->click();return;}if(stage==1&&!w.viewport->motionPaths.isEmpty()){stage=2;dialog->grab().save(QString("artifacts/motion-analysis-%1.png").arg(w.devicePixelRatioF()));dialog->reject();}});timer.start(100);w.showMotionAnalysis();timer.stop();QCOMPARE(stage,2);w.document().seek(15);captureEditorWidget(w.viewport).save(QString("artifacts/motion-paths-%1.png").arg(w.devicePixelRatioF()));for(auto &d:w.workspace.documents)d->undo.setClean();w.close();
    }
    void contextDragUndo() {
        Document d;d.replace(Scene::mannequin());auto result=EditService::execute(d,"motion_annotations",{{"operation","add"},{"annotation",QJsonObject{{"name","Support"},{"start",.5},{"end",1.5}}}});auto id=result["annotation"].toObject()["id"].toString();Timeline timeline(&d);timeline.resize(900,260);timeline.show();QVERIFY(QTest::qWaitForWindowExposed(&timeline));timeline.fit();QApplication::processEvents();int y=timeline.contextTrackCenter(id);QVERIFY(y>0);auto revision=d.motionRevision;int count=d.undo.count();QPoint from(qRound(timeline.framePosition(30)),y),to(qRound(timeline.framePosition(36)),y);QTest::mousePress(&timeline,Qt::LeftButton,Qt::NoModifier,from);QTest::mouseMove(&timeline,to);QTest::mouseRelease(&timeline,Qt::LeftButton,Qt::NoModifier,to);QCOMPARE(d.undo.count(),count+1);QCOMPARE(d.motionRevision,revision);QVERIFY(std::abs(d.scene.annotations[0].toObject()["start"].toDouble()-.7)<.02);d.undo.undo();QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),.5);
    }
    void atomDeformationUploads() {
        Scene scene=Scene::mannequin();auto &mesh=scene.meshes[0];mesh.node=0;
        for(auto &v:mesh.vertices){v.weights={.1f,.12f,.13f,.15f,.1f,.12f,.13f,.15f};v.joints={0,1,2,3,4,5,6,7};v.uv={v.p.x(),v.p.y()};}
        MorphTarget morph;morph.positions.resize(mesh.vertices.size(),{.03f,.02f,0});morph.normals.resize(mesh.vertices.size(),{.02f,0,.01f});mesh.targets={morph};scene.nodes[0].morphRest={0};scene.morphs[0]={.7f};
        QImage texture(8,8,QImage::Format_RGBA8888);texture.fill(QColor(120,170,210));Texture tex;QBuffer bytes(&tex.bytes);bytes.open(QIODevice::WriteOnly);texture.save(&bytes,"PNG");mesh.textures["baseColor"]=tex;
        scene.pose[1].r=QQuaternion::fromEulerAngles(12,16,7);scene.pose[1].s={.8f,1.2f,1.1f};
        Document document;document.replace(scene);Viewport view(&document);view.forceCpuSkinning=true;view.resize(640,640);view.show();QVERIFY(QTest::qWaitForWindowExposed(&view));QTest::qWait(100);
        auto before=view.grabFramebuffer();auto stats=view.renderingStats();
        QCOMPARE(stats["renderer"].toString(),QString("Atom"));
        QCOMPARE(stats["weightsPerVertex"].toInt(),8);
        QVERIFY(stats["cpuMeshes"].toInt()>0);
        document.scene.pose[1].r=QQuaternion::fromEulerAngles(35,-20,15);
        document.scene.morphs[0]={1.f};
        emit document.changed();
        auto after=view.grabFramebuffer();auto updated=view.renderingStats();
        QVERIFY(!before.isNull());QCOMPARE(before.size(),after.size());
        QCOMPARE(updated["meshHandles"],stats["meshHandles"]);
        QVERIFY(updated["skinUpdates"].toInteger()>stats["skinUpdates"].toInteger());
        int changedPixels=0;
        for(int y=0;y<before.height();++y) for(int x=0;x<before.width();++x)
            if(before.pixelColor(x,y)!=after.pixelColor(x,y)) ++changedPixels;
        before.save("artifacts/atom-deformation-before.png");after.save("artifacts/atom-deformation-after.png");
        QVERIFY(changedPixels>20);
    }
    void hiddenGeometryAndIndependentMaterials() {
        Document document;document.replace(Scene::mannequin());
        Viewport view(&document);view.resize(640,640);view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));QVERIFY(!view.grabFramebuffer().isNull());
        const auto initial=view.renderingStats();
        QTest::qWait(150);QCOMPARE(view.renderingStats()["geometryUpdates"],initial["geometryUpdates"]);
        view.showMesh=false;QTest::qWait(60);
        document.scene.pose[1].r=QQuaternion::fromEulerAngles(35,15,7);
        document.scene.meshes[0].color=Qt::red;++document.materialRevision;
        ++document.meshRevision;emit document.changed();QTest::qWait(100);
        QCOMPARE(view.renderingStats()["geometryUpdates"],initial["geometryUpdates"]);
        QVERIFY(!view.renderingStats()["meshVisible"].toBool());
        view.showMesh=true;QVERIFY(!view.grabFramebuffer().isNull());
        const auto shown=view.renderingStats();QVERIFY(shown["meshVisible"].toBool());
        QCOMPARE(shown["meshHandles"],initial["meshHandles"]);
        document.scene.meshes[0].color=Qt::green;++document.materialRevision;emit document.changed();
        QTest::qWait(100);QCOMPARE(view.renderingStats()["geometryUpdates"],shown["geometryUpdates"]);
        auto settings=document.scene.visuals;QString error;
        QVERIFY(settings.setValue("post.exposureEV",1,error));document.editVisual("Exposure",settings);
        QTest::qWait(100);QCOMPARE(view.renderingStats()["geometryUpdates"],shown["geometryUpdates"]);
        QVERIFY(view.renderingStats()["rhi"].toString().contains(qEnvironmentVariable("JESTER_RHI")));
        document.undo.setClean();
    }
    void atomSurfaceCloseAndReopen() {
        Window w;w.document().demo();w.resize(1100,720);w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));w.viewport->fit();
        QVERIFY(!w.viewport->grabFramebuffer().isNull());
        auto before=w.viewport->renderingStats();
        QVERIFY(before["loadedModels"].toInt()>0);
        for(int cycle=0;cycle<2;++cycle) {
            for(auto& document:w.workspace.documents) document->undo.setClean();
            QVERIFY(w.close());QTest::qWait(100);
            w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
            auto frame=w.viewport->grabFramebuffer();QVERIFY(!frame.isNull());
            auto after=w.viewport->renderingStats();
            QCOMPARE(after["meshHandles"],before["meshHandles"]);
            QCOMPARE(after["loadedModels"],before["loadedModels"]);
            QVERIFY(after["renderTicks"].toInteger()>before["renderTicks"].toInteger());
            before=after;
        }
        w.document().undo.setClean();w.close();QTest::qWait(100);
    }
    void referencedPathDisplay() {
        Window w;auto scene=Scene::mannequin();scene.physics.metersPerUnit=.01f;w.document().replace(scene);w.document().select(4);w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));int stage=0;QTimer timer;QElapsedTimer elapsed;elapsed.start();
        connect(&timer,&QTimer::timeout,&w,[&]{auto dialog=qobject_cast<QDialog*>(activeEditorModalWidget());if(!dialog)return;if(elapsed.elapsed()>10000){dialog->reject();return;}
            if(stage==0){dialog->findChild<QComboBox*>("trajectorySpace")->setCurrentText("root");dialog->findChild<QComboBox*>("trajectoryUnits")->setCurrentText("metres");for(auto button:dialog->findChildren<QPushButton*>())if(button->text()=="Calculate / update paths"){stage=1;button->click();return;}}
            if(stage==1&&!w.viewport->motionPaths.empty()){stage=2;dialog->reject();}});
        timer.start(100);w.showMotionAnalysis();timer.stop();QCOMPARE(stage,2);QVERIFY(w.viewport->motionPaths.contains(4));auto expected=w.document().scene.positions(w.document().scene.restPose())[4];QVERIFY((w.viewport->motionPaths[4].front().second-expected).length()<1e-5);QVERIFY(w.viewport->motionPathReferenceLabel.startsWith("root anchored"));w.document().undo.setClean();w.close();
    }
    void motionProblemNavigation() {
        Window w;Scene scene;QString error;QVERIFY2(loadProject("examples/motion-tools/penetration-and-contact.jst",scene,error),qPrintable(error));w.document().replace(scene);w.resize(qRound(1920/w.devicePixelRatioF()),qRound(1080/w.devicePixelRatioF()));w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));int stage=0;QTimer timer;QElapsedTimer elapsed;elapsed.start();
        connect(&timer,&QTimer::timeout,&w,[&]{auto dialog=qobject_cast<QDialog*>(activeEditorModalWidget());if(!dialog)return;if(elapsed.elapsed()>15000){dialog->reject();return;}
            if(stage==0){dialog->findChild<QComboBox*>("motionAnalysisMode")->setCurrentIndex(2);dialog->findChild<QCheckBox*>("motionSurfaceCheck")->setChecked(true);
                for(auto button:dialog->findChildren<QPushButton*>())if(button->text()=="Calculate / update paths"){stage=1;button->click();return;}}
            auto list=dialog->findChild<QListWidget*>("motionProblems");if(stage==1&&list&&list->count()>0){stage=2;list->setCurrentRow(0);
                for(int i=0;i<list->count();++i)if(list->item(i)->text().contains("surface-containment")){list->setCurrentRow(i);break;}
                for(auto tabs:dialog->findChildren<QTabWidget*>())for(int i=0;i<tabs->count();++i)if(tabs->tabText(i)=="Problems / intervals")tabs->setCurrentIndex(i);
                for(auto box:dialog->findChildren<QCheckBox*>())if(box->text()=="Transparent mesh")box->setChecked(true);
                captureEditorWidget(w.viewport).save(QString("artifacts/motion-diagnostic-viewport-%1.png").arg(w.devicePixelRatioF()));dialog->grab().save(QString("artifacts/motion-diagnostic-dialog-%1.png").arg(w.devicePixelRatioF()));dialog->reject();}});
        timer.start(100);w.showMotionAnalysis();timer.stop();QCOMPARE(stage,2);QVERIFY(!w.document().motionProblems.empty());QVERIFY(w.viewport->diagnosticOverlay);QCOMPARE(w.document().undo.count(),0);w.document().undo.setClean();w.close();
    }
    void largeCurveTablePaging() {
        Window w;auto scene=Scene::mannequin();ScalarCurve curve;curve.id="dense-curve";curve.node=0;
        for(int i=0;i<4097;++i){CurveKey key;key.time=i/1000.;key.value=i/1000.;key.interpolation="Linear";curve.keys.push_back(key);}scene.curves={curve};scene.curveBasePose=scene.pose;w.document().replace(scene);w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));bool seen=false;
        QTimer::singleShot(150,[&]{auto dialog=qobject_cast<QDialog*>(activeEditorModalWidget());if(!dialog)return;seen=true;
            auto table=dialog->findChild<QTableWidget*>("authoredCurveKeys");auto page=dialog->findChild<QSpinBox*>("curveKeyPage");QVERIFY(table);QVERIFY(page);QCOMPARE(table->rowCount(),512);page->setValue(9);QCOMPARE(table->rowCount(),1);QVERIFY(std::abs(table->item(0,0)->text().toDouble()-4.096)<1e-6);dialog->reject();});
        w.showGraphEditor();QVERIFY(seen);w.document().undo.setClean();w.close();
    }
    void compactMotionDialogs() {
        Window w;w.document().replace(Scene::mannequin());w.workspace.append(Scene::mannequin(),{},false);
        w.resize(qRound(1280/w.devicePixelRatioF()),qRound(720/w.devicePixelRatioF()));w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto inspect=[&](QString name,const std::function<void()> &open){bool seen=false;
            QTimer::singleShot(150,[&]{auto dialog=qobject_cast<QDialog*>(activeEditorModalWidget());if(!dialog)return;seen=true;
                QVERIFY2(dialog->width()*w.devicePixelRatioF()<=1280,qPrintable(name));QVERIFY2(dialog->height()*w.devicePixelRatioF()<=720,qPrintable(name));
                dialog->grab().save(QString("artifacts/motion-compact-%1-%2.png").arg(name).arg(w.devicePixelRatioF()));dialog->reject();});
            open();QVERIFY(seen);
        };
        w.timeline->editContext();QVERIFY(w.findChild<QDockWidget*>("notesDock")->isVisible());
        w.findChild<QDockWidget*>("notesDock")->hide();inspect("graph",[&]{w.showGraphEditor();});inspect("analysis",[&]{w.showMotionAnalysis();});inspect("retarget",[&]{w.showRetarget();});
        for(auto &d:w.workspace.documents)d->undo.setClean();w.close();
    }
    void nativeGraphEditing() {
        Window w;auto scene=Scene::mannequin();AnimationChannel channel;channel.node=0;channel.path="translation";channel.times={0,1};channel.values={0,1.04f,0,1,1.04f,0};
        AnimationClip clip;clip.duration=1;clip.channels={channel};scene.clips=std::make_shared<const std::vector<AnimationClip>>(std::vector<AnimationClip>{clip});scene.sourceClip=0;w.document().replace(scene);w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));bool extracted=false;
        QTimer::singleShot(100,[&]{auto dialog=qobject_cast<QDialog*>(activeEditorModalWidget());if(!dialog)return;
            for(auto tabs:dialog->findChildren<QTabWidget*>())for(int i=0;i<tabs->count();++i)if(tabs->tabText(i)=="Native source")tabs->setCurrentIndex(i);
            dialog->grab().save(QString("artifacts/motion-native-graph-%1.png").arg(w.devicePixelRatioF()));
            for(auto button:dialog->findChildren<QPushButton*>())if(button->text()=="Copy to authored override"){button->click();extracted=true;break;}dialog->reject();});
        w.showGraphEditor();QVERIFY(extracted);QCOMPARE(w.document().scene.curves.size(),size_t(1));w.document().undo.undo();QVERIFY(w.document().scene.curves.empty());w.document().undo.setClean();w.close();
    }
    void graphDialog() {
        Window w;w.document().replace(Scene::mannequin());w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));bool seen=false;
        QTimer::singleShot(100,[&]{auto d=qobject_cast<QDialog*>(activeEditorModalWidget());if(d){seen=true;for(auto b:d->findChildren<QPushButton*>())if(b->text()=="Create / select channel")b->click();d->grab().save("artifacts/motion-graph.png");d->reject();}});
        w.showGraphEditor();QVERIFY(seen);QCOMPARE(w.document().scene.curves.size(),size_t(1));
        Scene demo;QString error;QVERIFY2(loadProject("examples/motion-tools/curves-paths.jst",demo,error),qPrintable(error));w.document().replace(demo);auto before=curvesJson(w.document().scene.curves);
        QTimer::singleShot(150,[&]{auto dialog=qobject_cast<QDialog*>(activeEditorModalWidget());if(dialog){dialog->grab().save(QString("artifacts/motion-demo-graph-%1.png").arg(w.devicePixelRatioF()));dialog->reject();}});
        w.showGraphEditor();QCOMPARE(curvesJson(w.document().scene.curves),before);w.document().undo.setClean();w.close();
    }
    void contextDialog() {
        Window w;w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.timeline->editContext();QVERIFY(w.findChild<QDockWidget*>("notesDock")->isVisible());
        QVERIFY(!activeEditorModalWidget());QVERIFY(w.findChild<QListWidget*>("notesList"));
        w.document().undo.setClean();w.close();
    }
    void initTestCase() {
        QSettings().clear();
        QDir().mkpath("artifacts/mvp-ui");
    }
    void workflows_data() {
        QTest::addColumn<QString>("kind");
        for (auto kind : {"walk", "jump", "hand"})
            QTest::newRow(kind) << QString(kind);
    }
    void workflows() {
        QFETCH(QString, kind);
        QTemporaryDir temp;
        Window w;
        w.resize(1280, 720);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QString source = QFileInfo("examples/" + kind + "-correction.jst").absoluteFilePath();
        QFile original(source);
        QVERIFY(original.open(QIODevice::ReadOnly));
        auto sourceBytes = original.readAll();
        original.close();
        fileChoice(source);
        w.findChild<QAction *>("openAction")->trigger();
        QVERIFY(w.document().scene.name.contains(kind));
        QVERIFY(w.viewport->isValid());
        auto originalKeys = sceneToJson(w.document().scene)["keys"].toArray();
        auto motion = w.findChild<QDockWidget *>("motionDock");
        motion->show();
        motion->raise();
        auto profile = w.findChild<QPushButton *>("characterProfileButton");
        visible(w, profile);
        QTimer::singleShot(20, &w, [&] {
            auto dialog = w.findChild<QDialog *>("characterProfileDialog");
            QVERIFY(dialog);
            QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save),
                              Qt::LeftButton);
        });
        QTest::mouseClick(profile, Qt::LeftButton);
        QVERIFY(w.document().scene.profile.configured);
        selectNode(w, 8);
        QCOMPARE(w.document().selected, 8);
        auto position = w.findChild<QDoubleSpinBox *>("positionY");
        visible(w, position);
        auto before = w.document().scene.pose[8].t;
        int undo = w.document().undo.index();
        QTest::mouseClick(position, Qt::LeftButton);
        QTest::keyClick(position, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(position, QLocale().toString(before.y() + .025, 'f', 3));
        QTest::keyClick(position, Qt::Key_Return);
        QCOMPARE(w.document().undo.index(), undo + 1);
        QVERIFY((w.document().scene.pose[8].t - before).length() > .02f);
        QCOMPARE(sceneToJson(w.document().scene)["keys"].toArray(), originalKeys);
        auto compare = w.findChild<QCheckBox *>("compareOriginalCheck");
        visible(w, compare);
        auto revised = w.document().revision;
        QTest::mouseClick(compare, Qt::LeftButton, Qt::NoModifier, QPoint(8, compare->height() / 2));
        QVERIFY(w.document().scene.compareOriginal);
        QTest::mouseClick(compare, Qt::LeftButton, Qt::NoModifier, QPoint(8, compare->height() / 2));
        QVERIFY(!w.document().scene.compareOriginal);
        QCOMPARE(w.document().revision, revised);
        auto layers = w.findChild<QComboBox *>("motionLayers");
        QVERIFY(layers->count() >= 1);
        auto key = w.findChild<QPushButton *>("correctionKeyButton");
        visible(w, key);
        QTest::mouseClick(key, Qt::LeftButton);
        QVERIFY(w.document().scene.correction().keys.contains(w.document().frame));
        // Actual timeline gesture: copy the edited key to an empty frame, then undo.
        int from = w.document().frame, to = from + 2;
        w.timeline->fit();
        int oldCount = w.document().scene.correction().keys.size();
        QTest::mousePress(
            w.timeline, Qt::LeftButton, Qt::ControlModifier,
            QPoint(qRound(w.timeline->framePosition(from)), w.timeline->summaryTrackCenter(true)));
        QTest::mouseMove(w.timeline,
                         QPoint(qRound(w.timeline->framePosition(to)), w.timeline->summaryTrackCenter(true)),
                         30);
        QTest::mouseRelease(
            w.timeline, Qt::LeftButton, Qt::ControlModifier,
            QPoint(qRound(w.timeline->framePosition(to)), w.timeline->summaryTrackCenter(true)));
        QCOMPARE(w.document().scene.correction().keys.size(), oldCount + 1);
        w.document().undo.undo();
        if (kind != "hand") {
            bool checked = false;
            QTimer::singleShot(20, &w, [&] {
                auto dialog = w.findChild<QDialog *>("contactDialog");
                QVERIFY(dialog);
                auto list = dialog->findChild<QListWidget *>("contactList");
                QVERIFY(list->count() > 0);
                QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                                  list->visualItemRect(list->item(0)).center());
                QTest::mouseClick(dialog->findChild<QPushButton *>("contactUpdateButton"), Qt::LeftButton);
                QTest::mouseClick(dialog->findChild<QPushButton *>("diagnoseContactsButton"), Qt::LeftButton);
                checked = dialog->findChild<QLabel *>("contactFeedback")->text().contains("samples");
                dialog->grab().save("artifacts/mvp-ui/contacts-" +
                                    qEnvironmentVariable("QT_SCALE_FACTOR", "1") + ".png");
                QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close),
                                  Qt::LeftButton);
            });
            w.findChild<QAction *>("contactsAction")->trigger();
            QVERIFY(checked);
        }
        auto corrected = w.document().scene;
        QString saved = temp.path() + "/edited.jst";
        QAction *saveAs = nullptr;
        for (auto action : w.findChildren<QAction *>())
            if (action->text().startsWith("Save project as"))
                saveAs = action;
        QVERIFY(saveAs);
        fileChoice(saved);
        saveAs->trigger();
        QVERIFY(QFile::exists(saved));
        QVERIFY(!w.document().dirty());
        QTest::keyClick(&w, Qt::Key_W, Qt::ControlModifier);
        QTest::qWait(20);
        fileChoice(saved);
        w.findChild<QAction *>("openAction")->trigger();
        QCOMPARE(w.document().path, saved);
        for (float f : {0.f, 7.5f, 30.f, 45.f, 60.f}) {
            corrected.evaluate(f);
            auto copy = w.document().scene;
            copy.evaluate(f);
            auto a = corrected.positions(corrected.pose), b = copy.positions(copy.pose);
            for (size_t n = 0; n < a.size(); ++n)
                QVERIFY((a[n] - b[n]).length() < 1e-5f);
        }
        motion->raise();
        visible(w, w.findChild<QComboBox *>("motionLayers"));
        auto scroll = w.findChild<QScrollArea *>("motionScroll");
        scroll->verticalScrollBar()->setValue(0);
        QTest::qWait(60);
        qInfo()<<"Motion dock/scroll geometry"<<w.size()<<motion->geometry()<<scroll->viewport()->size();
        w.grab().save(qEnvironmentVariable("JESTER_VISUAL_TEST_OUTPUT","artifacts")+"/motion-layout-widgets.png");
        for (auto id : {"motionLayers", "correctionKeyButton", "compareOriginalCheck"}) {
            auto control = w.findChild<QWidget *>(id);
            QTRY_VERIFY2_WITH_TIMEOUT(control && !control->visibleRegion().isEmpty(),
                qPrintable(QString("%1 not visible; scroll viewport %2x%3, offset %4").arg(id)
                    .arg(scroll->viewport()->width()).arg(scroll->viewport()->height()).arg(scroll->verticalScrollBar()->value())),2000);
        }
        QDir().mkpath("artifacts/mvp-ui/" + kind);
        QVERIFY(captureEditorWidget(&w).save("artifacts/mvp-ui/" + kind + "/source-workspace-" +
                              qEnvironmentVariable("QT_SCALE_FACTOR", "1") + ".png"));
        auto rate = w.findChild<QSpinBox *>("motionBakeFps");
        visible(w, rate);
        rate->setValue(48);
        auto bake = w.findChild<QPushButton *>("bakeCorrectionButton");
        visible(w, bake);
        QTest::mouseClick(bake, Qt::LeftButton);
        QCOMPARE(w.document().scene.fps, 48);
        QVERIFY(w.document().scene.layers.empty());
        QString folder = "artifacts/mvp-ui/" + kind;
        QDir().mkpath(folder);
        for (QString extension : {"glb", "fbx"}) {
            QString path = QFileInfo(folder + "/result." + extension).absoluteFilePath();
            fileChoice(path);
            w.findChild<QAction *>("exportAction")->trigger();
            QVERIFY(QFile::exists(path));
        }
        QJsonArray reference;
        auto exported = w.document().scene;
        for (float f : {0.f, 12.f, 36.f, 60.f, 96.f}) {
            exported.evaluate(f);
            QJsonObject joints, transforms;
            QJsonArray geometry;
            auto positions = exported.positions(exported.pose);
            auto globals = exported.globals(exported.pose);
            for (size_t n = 0; n < positions.size(); ++n) {
                auto p = positions[n];
                joints[exported.nodes[n].name] = QJsonArray{p.x(), p.y(), p.z()};
                QJsonArray matrix;
                for (int i = 0; i < 16; ++i)
                    matrix.append(globals[n].constData()[i]);
                transforms[exported.nodes[n].name] = matrix;
            }
            for (const auto &mesh : exported.meshes)
                for (auto v : exported.skinned(mesh))
                    geometry.append(QJsonArray{v.x(), v.y(), v.z()});
            reference.append(QJsonObject{{"seconds", double(f) / 48},
                                         {"joints", joints},
                                         {"transforms", transforms},
                                         {"vertices", geometry}});
        }
        QFile expected(folder + "/expected.json");
        QVERIFY(expected.open(QIODevice::WriteOnly));
        expected.write(QJsonDocument(reference).toJson(QJsonDocument::Compact));
        expected.close();
        motion->raise();
        QTest::qWait(60);
        QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", "1");
        QVERIFY(captureEditorWidget(&w).save(folder + "/workspace-" + scale + ".png"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), sourceBytes);
        for (const auto &d : w.workspace.documents)
            d->undo.setClean();
        w.close();
    }
    void fullBodyPreviewFollowsSourceTab() {
        Window w;
        w.show();
        w.document().replace(mvpDemo("jump"));
        QString id = w.document().id;
        auto dock = w.findChild<QDockWidget *>("physicsDock");
        dock->show();
        dock->raise();
        auto simulate = w.findChild<QPushButton *>("simulatePhysicsButton");
        visible(w, simulate);
        QTest::mouseClick(simulate, Qt::LeftButton);
        w.newEmptyScene();
        auto tabs = w.findChild<QTabBar *>("sceneTabs");
        QCOMPARE(tabs->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(w.physicsTaskState()["done"].toBool(), 20000);
        QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
        QCOMPARE(w.document().id, id);
        QTRY_VERIFY_WITH_TIMEOUT(w.physicsResult().has_value(), 20000);
        QVERIFY(w.viewport->physicsPreview);
        auto source = w.document().scene;
        auto apply = w.findChild<QPushButton *>("applyPhysicsButton");
        visible(w, apply);
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(tabs->count(), 3);
        QVERIFY(w.document().id != id);
        QVERIFY(w.document().dirty());
        QVERIFY(w.document().scene.layers.empty());
        QCOMPARE(w.workspace.documents[0]->scene.layers.size(), source.layers.size());
        for (const auto &d : w.workspace.documents)
            d->undo.setClean();
        w.close();
    }
    void recoverThroughFileMenu() {
        QTemporaryDir temp;
        Workspace source;
        source.append(Scene::mannequin());
        source.active().seek(24);
        QString originalPath = temp.path() + "/original.jst", error;
        QVERIFY(saveProject(originalPath, source.active().scene, error));
        QFile original(originalPath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        auto before = original.readAll();
        original.close();
        source.active().path = originalPath;
        EditService::execute(source.active(), "pose_transform",
                             {{"node", 0}, {"position", QJsonArray{.37, 1.04, 0}}});
        RecoveryStore snapshots(source, temp.path() + "/recovery");
        QVERIFY(snapshots.flush(error));
        Window w;
        w.show();
        fileChoice(snapshots.snapshots().front());
        w.findChild<QAction *>("recoverAutosaveAction")->trigger();
        QVERIFY(w.document().path.isEmpty());
        QVERIFY(w.document().dirty());
        QCOMPARE(w.document().frame, 24);
        QVERIFY(std::abs(w.document().scene.pose[0].t.x() - .37f) < 1e-5f);
        QString recovered = temp.path() + "/recovered.jst";
        fileChoice(recovered);
        for (auto action : w.findChildren<QAction *>())
            if (action->text().startsWith("Save project as")) {
                action->trigger();
                break;
            }
        QVERIFY(QFile::exists(recovered));
        QVERIFY(!w.document().dirty());
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), before);
        for (const auto &d : w.workspace.documents)
            d->undo.setClean();
        w.close();
    }
    void uiAndApiParity() {
        Window w;
        w.show();
        w.document().replace(Scene::mannequin());
        selectNode(w, 8);
        EditorApi api(&w);
        auto start = w.document().scene.positions(w.document().scene.pose)[8];
        auto box = w.findChild<QDoubleSpinBox *>("positionY");
        visible(w, box);
        QTest::mouseClick(box, Qt::LeftButton);
        QTest::keyClick(box, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(box, QLocale().toString(start.y() + .1, 'f', 3));
        QTest::keyClick(box, Qt::Key_Return);
        auto ui = w.document().scene;
        int count = w.document().undo.count();
        w.document().undo.undo();
        auto result =
            api.call("pose_transform", {{"node", 8},
                                        {"position", QJsonArray{start.x(), start.y() + .1, start.z()}},
                                        {"space", "world"}});
        QVERIFY(!result["isError"].toBool());
        QCOMPARE(w.document().undo.count(), count);
        for (size_t n = 0; n < ui.pose.size(); ++n)
            QVERIFY((ui.pose[n].t - w.document().scene.pose[n].t).length() < 1e-5f);
        w.document().undo.setClean();
        w.close();
    }
};
#include "atom_test_main.h"
int main(int argc,char** argv) { return runJesterTests<MVPUI>(argc,argv,"JesterMvpTests"); }
#include "mvp_ui_tests.moc"
