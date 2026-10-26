#ifndef Q_MOC_RUN
#include "render/atom/AtomViewport.h"
#endif
#include "io/project/project_io.h"
#include "ui/window/window.h"
#include "automation/editor_api.h"
#include "settings/preferences_service.h"
#include "settings/localization.h"
#include "settings/action_registry.h"
#include "ui/dialogs/preferences_dialog.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <cmath>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFile>
#include <QFontInfo>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTableWidget>
#include <QScreen>
#include <QToolBar>
#include <QMenuBar>
#include <QKeySequenceEdit>
#include "ui/widgets/localized_ui.h"
#include <QtTest>

using namespace trickster;
namespace {
double imageDifference(const QImage &a,const QImage &b) {
    if(a.isNull() || a.size()!=b.size())return 1000;
    double error=0;
    for(int y=0;y<a.height();++y)for(int x=0;x<a.width();++x) {
        const auto p=a.pixelColor(x,y),q=b.pixelColor(x,y);
        error+=std::abs(p.red()-q.red())+std::abs(p.green()-q.green())+std::abs(p.blue()-q.blue());
    }
    return error/(a.width()*a.height()*3.);
}
QJsonObject patch(EditorApi &api,const QJsonObject &settings) {
    return api.call("scene_visuals",{{"operation","set"},{"settings",settings}});
}
Scene eightInfluenceFixture() {
    auto scene=Scene::mannequin();auto &mesh=scene.meshes[0];mesh.node=0;mesh.color=QColor("#4ab880");
    mesh.metallic=.2f;mesh.roughness=.5f;
    for(auto &vertex:mesh.vertices) {vertex.weights={.1f,.12f,.13f,.15f,.1f,.12f,.13f,.15f};vertex.joints={0,1,2,3,4,5,6,7};}
    MorphTarget target;target.name="Authored position normal tangent delta";
    for(const auto &vertex:mesh.vertices) {
        const float wave=std::sin(vertex.p.y()*3.f);
        target.positions.push_back({.03f*wave,.02f,.01f});target.normals.push_back({.01f,0,.01f});target.tangents.push_back({0,.01f,0});
    }
    mesh.targets={target};scene.nodes[0].morphRest={0};scene.morphs[0]={.65f};
    scene.pose[1].r=QQuaternion::fromEulerAngles(18,25,7);scene.pose[1].s={.8f,1.2f,1.1f};
    scene.pose[5].r=QQuaternion::fromEulerAngles(-12,7,25);
    QString error;scene.visuals.patch({{"mode","unlit"},{"grid",QJsonObject{{"enabled",false},{"axes",false}}},{"floor",QJsonObject{{"enabled",false}}}},error);
    return scene;
}
}
class VisualTests:public QObject {
    Q_OBJECT
  private slots:
    void gpuMatchesCpuEightWeightsAndMorphs() {
        Document document;document.replace(eightInfluenceFixture());Viewport view(&document);view.showBones=false;
        view.resize(720,640);view.show();QVERIFY(QTest::qWaitForWindowExposed(&view));view.fit();
        auto gpu=view.grabFramebuffer();QVERIFY(!gpu.isNull());
        const auto gpuStats=view.renderingStats();QCOMPARE(gpuStats["skinningPath"].toString(),QString("gpu"));
        QVERIFY(gpuStats["gpuMeshes"].toInt()>0);QCOMPARE(gpuStats["weightsPerVertex"].toInt(),8);
#ifndef Q_MOC_RUN
        AtomViewport *renderer=nullptr;
        for(auto *child:view.findChildren<QObject*>())if(auto *candidate=dynamic_cast<AtomViewport*>(child))renderer=candidate;
        QVERIFY(renderer);const auto numerical=renderer->validateSkinningReference();
        const auto output=qEnvironmentVariable("JESTER_VISUAL_TEST_OUTPUT","artifacts/visual-tests");QDir().mkpath(output);
        QFile evidence(output+"/skinning-comparison.json");QVERIFY(evidence.open(QIODevice::WriteOnly));evidence.write(QJsonDocument(numerical).toJson());evidence.close();
        QVERIFY2(!numerical.contains("error"),qPrintable(QJsonDocument(numerical).toJson()));
        const auto comparisons=numerical["meshes"].toArray();QVERIFY(!comparisons.isEmpty());
        for(const auto &value:comparisons) {
            const auto comparison=value.toObject();QVERIFY(comparison["finiteAndComplete"].toBool());
            QVERIFY2(comparison["positionMaxComponentError"].toDouble()<.001,qPrintable(QJsonDocument(comparison).toJson()));
            for(const auto &field:{"normalMaxComponentError","tangentMaxComponentError","bitangentMaxComponentError"})
                QVERIFY2(comparison[field].toDouble()<.04,qPrintable(QJsonDocument(comparison).toJson()));
        }
#endif
        view.forceCpuSkinning=true;auto cpu=view.grabFramebuffer();QVERIFY(!cpu.isNull());
        QCOMPARE(view.renderingStats()["skinningPath"].toString(),QString("cpu"));
        const auto delta=imageDifference(gpu,cpu);qInfo()<<"Eight-weight morph CPU/GPU framebuffer mean absolute channel error"<<delta;
        QVERIFY2(delta<4.,qPrintable(QString::number(delta)));
        int greenPixels=0;
        for(int y=0;y<gpu.height();++y)for(int x=0;x<gpu.width();++x) {const auto c=gpu.pixelColor(x,y);if(c.green()>c.red()+15 && c.green()>c.blue()+10)++greenPixels;}
        QVERIFY(greenPixels>100);
        gpu.save(output+"/gpu-eight-weights-morph.png");cpu.save(output+"/cpu-eight-weights-morph.png");
    }
    void modesUseOneModelAndUnlitIgnoresLighting() {
        Window window;window.document().replace(eightInfluenceFixture());window.resize(1280,720);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));window.viewport->fit();
        EditorApi api(&window);const auto original=sceneToJson(window.document().scene);
        const auto unlit=window.viewport->grabFramebuffer();QVERIFY(!unlit.isNull());
        auto result=patch(api,{{"lighting",QJsonObject{{"main",QJsonObject{{"intensityLux",10000}}}}},{"environment",QJsonObject{{"exposureEV",12}}},{"post",QJsonObject{{"exposureEV",6},{"bloom",QJsonObject{{"enabled",true},{"intensity",2}}}}}});
        QVERIFY2(!result["isError"].toBool(),qPrintable(QJsonDocument(result).toJson()));
        auto changed=window.viewport->grabFramebuffer();QVERIFY(imageDifference(unlit,changed)<.2);
        auto *wire=window.findChild<QAction*>("viewportMode_wireframe");QVERIFY(wire);wire->trigger();QVERIFY(wire->isChecked());
        QCOMPARE(window.document().scene.visuals.value("mode").toString(),QString("wireframe"));
        const auto wireImage=window.viewport->grabFramebuffer();QVERIFY(imageDifference(unlit,wireImage)>1.);
        auto *color=window.findChild<QPushButton*>("sceneSetting_wireframe.color");QVERIFY(color);
        const auto state=api.call("scene_visuals",{{"operation","get"}})["structuredContent"].toObject();
        QCOMPARE(state["authored"].toObject()["mode"].toString(),QString("wireframe"));
        QCOMPARE(state["applied"].toObject()["mode"].toString(),QString("wireframe"));
        const auto current=sceneToJson(window.document().scene);
        for(const auto &field:{"keys","curves","layers","meshes","annotations"})QCOMPARE(current[field],original[field]);
        const auto count=window.document().undo.count();const auto revision=window.document().motionRevision;
        result=patch(api,{{"lighting",QJsonObject{{"fill",QJsonObject{{"shadows",true}}}}}});
        QVERIFY(result["isError"].toBool());QCOMPARE(window.document().undo.count(),count);QCOMPARE(window.document().motionRevision,revision);
        window.document().undo.setClean();window.close();
    }
    void cpuFallbackBuildsActualWireEdgesOnModeChange() {
        Document document;document.replace(eightInfluenceFixture());Viewport view(&document);view.forceCpuSkinning=true;view.showBones=false;
        view.resize(720,640);view.show();QVERIFY(QTest::qWaitForWindowExposed(&view));view.fit();QVERIFY(!view.grabFramebuffer().isNull());
        auto settings=document.scene.visuals;QString error;
        QVERIFY(settings.setValue("mode","wireframe",error));QVERIFY(settings.setValue("wireframe.color","#ff0033",error));
        document.editVisual("Wireframe",settings);const auto image=view.grabFramebuffer();QVERIFY(!image.isNull());
        int edges=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x) {
            auto color=image.pixelColor(x,y);if(color.red()>color.green()+60 && color.red()>color.blue()+60)++edges;
        }
        QVERIFY2(edges>50,qPrintable(QString::number(edges)));
        const auto count=view.renderingStats()["geometryUpdates"];
        QTest::qWait(100);QCOMPARE(view.renderingStats()["geometryUpdates"],count);
    }
    void lightingPostAndBackgroundAffectNativeImage() {
        Window window;auto scene=Scene::mannequin();scene.meshes[0].color=QColor("#4ab880");scene.visuals=SceneVisualSettings();
        QString error;scene.visuals.patch({{"grid",QJsonObject{{"enabled",false},{"axes",false}}}},error);
        window.document().replace(scene);window.viewport->showBones=false;window.resize(1280,720);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));window.viewport->fit();
        EditorApi api(&window);const auto initial=window.viewport->grabFramebuffer();QVERIFY(!initial.isNull());
        QVERIFY(!patch(api,{{"post",QJsonObject{{"exposureEV",-6}}}})["isError"].toBool());
        const auto exposed=window.viewport->grabFramebuffer();QVERIFY(imageDifference(initial,exposed)>.2);
        QVERIFY(!patch(api,{{"post",QJsonObject{{"toneMapper","reinhard"},{"saturation",0},{"contrast",1.3}}}})["isError"].toBool());
        const auto graded=window.viewport->grabFramebuffer();QVERIFY(imageDifference(exposed,graded)>.2);
        QVERIFY(!patch(api,{{"post",QJsonObject{{"bloom",QJsonObject{{"enabled",true},{"threshold",.1},{"intensity",1.5}}}}}})["isError"].toBool());
        const auto bloom=window.viewport->grabFramebuffer();QVERIFY(imageDifference(graded,bloom)>.05);
        // Native sky is part of scene post processing. Isolate the background
        // change from the preceding zero-saturation/Bloom test.
        QVERIFY(!patch(api,{{"post",QJsonObject{{"saturation",1},{"contrast",1},{"bloom",QJsonObject{{"enabled",false}}}}}})["isError"].toBool());
        const auto beforeBackground=window.viewport->grabFramebuffer();
        QVERIFY(!patch(api,{{"background",QJsonObject{{"mode","gradient"},{"topColor","#602030"},{"bottomColor","#1040a0"}}}})["isError"].toBool());
        const auto background=window.viewport->grabFramebuffer();QVERIFY(imageDifference(beforeBackground,background)>1.);
        QVERIFY(!patch(api,{{"post",SceneVisualSettings().toJson()["post"]}})["isError"].toBool());
        const auto neutral=window.viewport->grabFramebuffer();
        QVERIFY(!patch(api,{{"post",QJsonObject{{"enabled",false}}}})["isError"].toBool());
        const auto disabled=window.viewport->grabFramebuffer();
        qInfo()<<"effect differences"<<imageDifference(initial,exposed)<<imageDifference(exposed,graded)<<imageDifference(graded,bloom)<<imageDifference(beforeBackground,background)<<imageDifference(neutral,disabled);
        const auto output=qEnvironmentVariable("JESTER_VISUAL_TEST_OUTPUT","artifacts/visual-tests");QDir().mkpath(output);
        initial.save(output+"/lit-default.png");exposed.save(output+"/exposure.png");graded.save(output+"/graded.png");bloom.save(output+"/bloom.png");background.save(output+"/gradient.png");disabled.save(output+"/disabled.png");
        QVERIFY(imageDifference(neutral,disabled)>.2);
        window.document().undo.setClean();window.close();
    }
    void sceneSliderIsOneVisualUndoAndSurvivesSave() {
        Window window;window.document().replace(Scene::mannequin());window.resize(1280,720);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *dock=window.findChild<QDockWidget*>("sceneSettingsDock");QVERIFY(dock);dock->show();dock->raise();
        auto *slider=window.findChild<QSlider*>("sceneSlider_lighting.main.azimuth");QVERIFY(slider);
        const auto count=window.document().undo.count();const auto motion=window.document().motionRevision;
        slider->setSliderDown(true);slider->setValue(100);slider->setValue(200);slider->setValue(300);slider->setSliderDown(false);
        QCOMPARE(window.document().undo.count(),count+1);QCOMPARE(window.document().motionRevision,motion);
        const auto settings=window.document().scene.visuals;
        QTemporaryDir directory;QString error;const auto path=directory.filePath("visual project.jst");
        QVERIFY2(saveProject(path,window.document().scene,error),qPrintable(error));Scene loaded;QVERIFY(loadProject(path,loaded,error));QCOMPARE(loaded.visuals.toJson(),settings.toJson());
        window.document().undo.undo();QVERIFY(window.document().scene.visuals!=settings);window.document().undo.redo();QCOMPARE(window.document().scene.visuals.toJson(),settings.toJson());
        window.document().undo.setClean();window.close();
    }
    void nativeLightsEnvironmentShadowsAndSequence() {
        Window window;auto scene=Scene::mannequin();scene.visuals=SceneVisualSettings();
        for(auto &mesh:scene.meshes) {mesh.metallic=.85f;mesh.roughness=.3f;}
        window.document().replace(scene);window.viewport->showBones=false;
        window.resize(1280,720);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));window.viewport->fit();
        EditorApi api(&window);const auto motion=window.document().motionRevision;
        const auto output=qEnvironmentVariable("JESTER_VISUAL_TEST_OUTPUT","artifacts/visual-tests");
        const auto lit=window.viewport->grabFramebuffer();
        QVERIFY(!patch(api,{{"lighting",QJsonObject{{"main",QJsonObject{{"enabled",false}}},{"fill",QJsonObject{{"enabled",false}}}}}})["isError"].toBool());
        const auto ambient=window.viewport->grabFramebuffer();QVERIFY(imageDifference(lit,ambient)>.1);
        QVERIFY(!patch(api,{{"environment",QJsonObject{{"exposureEV",3}}}})["isError"].toBool());
        const auto brighter=window.viewport->grabFramebuffer();
        QVERIFY2(imageDifference(ambient,brighter)>.02,qPrintable(QString::number(imageDifference(ambient,brighter))));
        QVERIFY(!patch(api,{{"environment",QJsonObject{{"rotation",90}}}})["isError"].toBool());
        const auto rotated=window.viewport->grabFramebuffer();QVERIFY(imageDifference(brighter,rotated)>.01);
        QVERIFY(!patch(api,{{"environment",QJsonObject{{"enabled",false}}}})["isError"].toBool());
        const auto dark=window.viewport->grabFramebuffer();QVERIFY(imageDifference(rotated,dark)>.02);
        QVERIFY(!patch(api,{{"lighting",QJsonObject{{"main",QJsonObject{{"enabled",true},{"shadows",true}}}}}})["isError"].toBool());
        const auto shadowed=window.viewport->grabFramebuffer();
        QVERIFY(!patch(api,{{"lighting",QJsonObject{{"main",QJsonObject{{"shadows",false}}}}}})["isError"].toBool());
        const auto noShadows=window.viewport->grabFramebuffer();
        lit.save(output+"/metal-studio.png");ambient.save(output+"/metal-ambient.png");brighter.save(output+"/ibl-brightness.png");
        rotated.save(output+"/ibl-rotation.png");shadowed.save(output+"/shadows-on.png");noShadows.save(output+"/shadows-off.png");
        qInfo()<<"Native image differences lights/IBL EV/rotation/shadows"<<imageDifference(lit,ambient)<<imageDifference(ambient,brighter)<<imageDifference(brighter,rotated)<<imageDifference(shadowed,noShadows);
        QVERIFY(imageDifference(shadowed,noShadows)>.01);
        QCOMPARE(window.document().motionRevision,motion);
        QTemporaryDir directory;window.document().seek(0);
        const auto still=directory.filePath("still.png");
        QVERIFY(!api.call("screenshot",{{"target","viewport"},{"path",still},{"overlays",false}})["isError"].toBool());
        QVERIFY(!api.call("record_frames",{{"directory",directory.path()},{"first",0},{"last",0},{"target","viewport"},{"overlays",false}})["isError"].toBool());
        QVERIFY(imageDifference(QImage(still),QImage(directory.filePath("frame-00000.png")))<.25);
        const auto renderer=window.viewport->renderingStats();QCOMPARE(renderer["cameraSource"].toString(),QString("editor"));QVERIFY(renderer["cameraDistance"].isNull());
        window.document().undo.setClean();window.close();
    }
    void missingNormalsAreDerivedWithoutEditingAuthoredMesh() {
        auto scene=Scene::mannequin();for(auto &mesh:scene.meshes)for(auto &vertex:mesh.vertices)vertex.normal={};
        const auto original=sceneToJson(scene);Document document;document.replace(scene);
        Viewport view(&document);view.showBones=false;view.resize(720,640);view.show();QVERIFY(QTest::qWaitForWindowExposed(&view));view.fit();
        const auto gpu=view.grabFramebuffer();QVERIFY(!gpu.isNull());
#ifndef Q_MOC_RUN
        AtomViewport *renderer=nullptr;
        for(auto *child:view.findChildren<QObject*>())if(auto *candidate=dynamic_cast<AtomViewport*>(child))renderer=candidate;
        QVERIFY(renderer);const auto reference=renderer->validateSkinningReference();
        QVERIFY2(!reference.contains("error"),qPrintable(QJsonDocument(reference).toJson()));
        QVERIFY(!reference["meshes"].toArray().isEmpty());
        for(auto value:reference["meshes"].toArray())QVERIFY(value.toObject()["normalMaxComponentError"].toDouble(100)<.001);
#endif
        view.forceCpuSkinning=true;const auto cpu=view.grabFramebuffer();QVERIFY(imageDifference(gpu,cpu)<1);
        QCOMPARE(sceneToJson(document.scene),original);
    }
    void localeDraftCommitAndOpenMessageFollowLanguage() {
        auto &service=PreferencesService::instance();const auto baseline=service.current();QString error;
        auto preferences=baseline;QVERIFY(preferences.patch({{"language","en"}},error));QVERIFY(service.preview(preferences,error));
        Window window;window.document().replace(Scene::mannequin());window.document().select(0);
        window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *number=window.findChild<QDoubleSpinBox*>("positionY");auto *edit=number->findChild<QLineEdit*>();
        edit->setText(number->prefix()+"0.123"+number->suffix());edit->setModified(true);
        QVERIFY(preferences.patch({{"language","ru"},{"appearance",QJsonObject{{"fontFamily","Arial"}}}},error));QVERIFY(service.preview(preferences,error));
        QVERIFY2(edit->text().contains("0,123"),qPrintable(edit->text()));
        QCOMPARE(QFontInfo(number->font()).family(),QString("Arial"));
        QVERIFY(preferences.patch({{"language","en"}},error));QVERIFY(service.preview(preferences,error));
        QVERIFY(edit->text().contains("0.123"));number->interpretText();QVERIFY(std::abs(number->value()-.123)<.00001);
        bool translated=false;
        QTimer::singleShot(8000,&window,[] {if(auto *box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->reject();});
        QTimer::singleShot(30,&window,[&] {
            QVERIFY(preferences.patch({{"language","ru"}},error));QVERIFY(service.preview(preferences,error));
            auto *box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());QVERIFY(box);
            translated=box->windowTitle()==uiText("Joint limit") && box->text()==uiText("Select a joint first.");box->done(QMessageBox::Ok);
        });
        showUiMessage(QMessageBox::Information,&window,[]{return uiText("Joint limit");},[]{return uiText("Select a joint first.");});QVERIFY(translated);
        const auto path=QString::fromUtf8("C:/Персонаж %2/scene.jst");
        QVERIFY(translatedDiagnostic("Cannot create autosave directory: "+path).endsWith(path));
        QCOMPARE(roleSourceName("custom-Scene"),QString("custom-Scene"));
        QTableWidget roles;roles.setColumnCount(1);roles.setRowCount(1);setRoleItem(&roles,0,"Scene");QCOMPARE(roles.item(0,0)->text(),QString("Scene"));
        QVERIFY(uiText(roleSourceName("leftHand"))!=QString("leftHand"));
        QVERIFY(service.preview(baseline,error));window.document().undo.setClean();window.close();
    }
    void orthographicFrameAndCompactNativeCapture() {
        Window window;window.document().replace(Scene::mannequin());
        window.resize(qRound(1280/window.devicePixelRatioF()),qRound(720/window.devicePixelRatioF()));window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));EditorApi api(&window);
        QVERIFY(!patch(api,{{"camera",QJsonObject{{"projection","orthographic"},{"orthographicHeight",.1}}}})["isError"].toBool());
        const auto motion=window.document().motionRevision;window.viewport->frameCharacter();
        QVERIFY(window.document().scene.visuals.value("camera.orthographicHeight").toDouble()>1);
        QCOMPARE(window.document().motionRevision,motion);
        auto bounds=window.document().scene.bounds();
        for(auto point:{bounds.first,bounds.second})QVERIFY(window.viewport->rect().contains(window.viewport->project(point).toPoint()));
        auto frame=window.viewport->grabFramebuffer();QVERIFY(!frame.isNull());
        AtomViewport *native=nullptr;
        for(auto *child:window.viewport->findChildren<QWidget*>())if(auto *candidate=dynamic_cast<AtomViewport*>(child))native=candidate;
        QVERIFY(native);QCOMPARE(native->geometry(),window.viewport->rect());
        const auto expected=window.viewport->size()*window.viewport->devicePixelRatioF();
        // Fractional DPI rounds the native rectangle edges independently.
        QVERIFY(std::abs(frame.width()-expected.width())<=1);
        QVERIFY(std::abs(frame.height()-expected.height())<=1);
        window.showMinimized();QTest::qWait(40);window.showNormal();QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(!window.viewport->grabFramebuffer().isNull());
        const auto output=qEnvironmentVariable("JESTER_VISUAL_TEST_OUTPUT","artifacts/visual-tests");
        QVERIFY(captureEditorWidget(&window).save(output+"/compact-native.png"));
        window.document().undo.setClean();window.close();
    }
    void preferencePreviewCancelPreservesDraftAndActionIds() {
        Window window;window.document().replace(Scene::mannequin());window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *draft=window.findChild<QDoubleSpinBox*>("positionY");QVERIFY(draft);
        const auto document=sceneToJson(window.document().scene),preferences=PreferencesService::instance().current().toJson();
        const auto undo=window.document().undo.count(),frame=window.document().frame;
        ActionRegistry registry(window);QStringList before;for(auto *action:registry.actions())before<<ActionRegistry::id(action);
        QTimer::singleShot(10000,&window,[&window] {if(auto *dialog=window.findChild<QDialog*>("preferencesDialog"))dialog->reject();});
        int stage=0;QTimer timer;QObject::connect(&timer,&QTimer::timeout,&window,[&] {
            auto *dialog=window.findChild<QDialog*>("preferencesDialog");if(!dialog)return;
            if(stage==0) {
                auto *actions=dialog->findChild<QTreeWidget*>("preferencesActions");QVERIFY(actions);actions->setCurrentItem(actions->topLevelItem(0));
                dialog->findChild<QKeySequenceEdit*>("preferencesShortcutEdit")->setKeySequence(QKeySequence("Ctrl+Alt+9"));
                auto *language=dialog->findChild<QComboBox*>("preference_language");language->setCurrentIndex(language->findData("ru"));
                stage=1;dialog->findChild<QPushButton*>("preferencesPreview")->click();
            } else if(stage==1) {
                QCOMPARE(LocalizationService::instance().language(),QString("ru"));
                QCOMPARE(dialog->windowTitle(),QString::fromUtf8("Настройки"));
                QVERIFY(LocalizationService::instance().qtTranslationLoaded());stage=2;dialog->reject();
                QCOMPARE(dialog->findChild<QKeySequenceEdit*>("preferencesShortcutEdit")->keySequence(),QKeySequence("Ctrl+Alt+9"));
            }
        });timer.start(30);showPreferences(window);timer.stop();QCOMPARE(stage,2);
        QCOMPARE(PreferencesService::instance().current().toJson(),preferences);QCOMPARE(sceneToJson(window.document().scene),document);
        QCOMPARE(window.document().undo.count(),undo);QCOMPARE(window.document().frame,frame);
        QStringList after;for(auto *action:ActionRegistry(window).actions())after<<ActionRegistry::id(action);after.sort();before.sort();QCOMPARE(after,before);
        window.document().undo.setClean();window.close();
    }
    void liveLanguageThemesKeepNamesDraftsAndEnums() {
        auto &service=PreferencesService::instance();const auto baseline=service.current();
        QString error;auto startup=baseline;QVERIFY(startup.patch({{"language","ru"}},error));QVERIFY(service.preview(startup,error));
        Window window;auto scene=Scene::mannequin();scene.nodes[0].name="Scene";
        window.document().replace(scene);window.document().select(0);
        window.resize(1280,720);window.show();window.raise();window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QStringList menus;for(auto *action:window.menuBar()->actions())menus<<uiSourceText(action);
        for(const auto &source:{QString("&File"),QString("&Edit"),QString("&View"),QString("&Help"),QString("&Scene")})QVERIFY2(menus.contains(source),qPrintable(menus.join(" / ")));
        QCOMPARE(menus.count("&Scene"),1);
        auto *number=window.findChild<QDoubleSpinBox*>("positionY");QVERIFY(number);
        auto *numericDraft=number->findChild<QLineEdit*>();QVERIFY(numericDraft);
        window.findChild<QPushButton*>("addNoteButton")->click();
        auto *note=window.findChild<QTextEdit*>("noteText");QVERIFY(note);
        const auto authored=sceneToJson(window.document().scene);const auto undo=window.document().undo.count();const auto tool=window.viewport->tool;
        note->setPlainText(QString::fromUtf8("Черновик Scene / Open, без перевода"));note->setFocus();QTest::keyClicks(note," weip q ");
        const auto noteDraft=note->toPlainText();QCOMPARE(window.viewport->tool,tool);QCOMPARE(sceneToJson(window.document().scene),authored);
        auto *customType=window.findChild<QComboBox*>("noteType");QVERIFY(customType);customType->setEditText("Scene");customType->lineEdit()->setModified(false);
        numericDraft->setText("0.123456");numericDraft->setModified(true);
        ActionRegistry registry(window);QStringList ids;for(auto *action:registry.actions())ids<<ActionRegistry::id(action);ids.sort();
        const auto output=qEnvironmentVariable("JESTER_VISUAL_TEST_OUTPUT","artifacts/visual-tests");QDir().mkpath(output);
        for(const auto &theme:{QString("light"),QString("high-contrast"),QString("dark")}) {
            auto preferences=baseline;
            QVERIFY(preferences.patch({{"language","ru"},{"appearance",QJsonObject{{"theme",theme},{"fontSize",11},{"iconSize",24},{"uiScale",1.1}}}},error));
            QVERIFY2(service.preview(preferences,error),qPrintable(error));
            QCOMPARE(window.findChild<QDockWidget*>("skeletonDock")->windowTitle(),QString::fromUtf8("Структура"));
            QCOMPARE(window.findChild<QAction*>("saveAction")->text(),QString::fromUtf8("Сохранить проект"));
            QCOMPARE(window.document().scene.nodes[0].name,QString("Scene"));
            QCOMPARE(sceneToJson(window.document().scene),authored);QCOMPARE(window.document().undo.count(),undo);
            QCOMPARE(note->toPlainText(),noteDraft);QCOMPARE(numericDraft->text(),QString("0.123456"));
            auto *origin=window.findChild<QComboBox*>("noteOrigin");QVERIFY(origin);
            QCOMPARE(choiceText(origin),QString("intention"));QCOMPARE(origin->currentData().toString(),QString("intention"));
            QCOMPARE(choiceText(customType),QString("Scene"));QCOMPARE(customType->currentText(),QString("Scene"));
            QCOMPARE(window.findChild<QToolBar*>("mainToolbar")->iconSize(),QSize(26,26));
            QCOMPARE(uiText("%n action(s)",1),QString::fromUtf8("1 действие"));
            QCOMPARE(uiText("%n action(s)",2),QString::fromUtf8("2 действия"));
            QCOMPARE(uiText("%n action(s)",5),QString::fromUtf8("5 действий"));
            QTest::qWait(100);window.grab().save(output+"/ru-"+theme+"-widgets.png");
        }
        auto preferences=baseline;QVERIFY(preferences.patch({{"language","en"}},error));QVERIFY(service.preview(preferences,error));
        QCOMPARE(window.findChild<QDockWidget*>("skeletonDock")->windowTitle(),QString("Outliner"));
        QCOMPARE(uiText("%n action(s)",1),QString("1 action"));QCOMPARE(uiText("%n action(s)",2),QString("2 actions"));
        QCOMPARE(note->toPlainText(),noteDraft);QCOMPARE(numericDraft->text(),QString("0.123456"));
        QCOMPARE(choiceText(customType),QString("Scene"));QCOMPARE(customType->currentText(),QString("Scene"));
        QStringList after;for(auto *action:ActionRegistry(window).actions())after<<ActionRegistry::id(action);after.sort();QCOMPARE(after,ids);
        window.findChild<QPushButton*>("noteCancel")->click();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QVERIFY(service.preview(baseline,error));window.document().undo.setClean();window.close();
    }
};
#include "atom_test_main.h"
int main(int argc,char **argv) {return runJesterTests<VisualTests>(argc,argv,"JesterVisualTests");}
#include "visual_tests.moc"
