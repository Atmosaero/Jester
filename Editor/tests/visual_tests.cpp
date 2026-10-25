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
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
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
        QVERIFY(!patch(api,{{"background",QJsonObject{{"mode","gradient"},{"topColor","#602030"},{"bottomColor","#1040a0"}}}})["isError"].toBool());
        const auto background=window.viewport->grabFramebuffer();QVERIFY(imageDifference(bloom,background)>1.);
        QVERIFY(!patch(api,{{"post",SceneVisualSettings().toJson()["post"]}})["isError"].toBool());
        const auto neutral=window.viewport->grabFramebuffer();
        QVERIFY(!patch(api,{{"post",QJsonObject{{"enabled",false}}}})["isError"].toBool());
        const auto disabled=window.viewport->grabFramebuffer();
        qInfo()<<"effect differences"<<imageDifference(initial,exposed)<<imageDifference(exposed,graded)<<imageDifference(graded,bloom)<<imageDifference(bloom,background)<<imageDifference(neutral,disabled);
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
                auto *language=dialog->findChild<QComboBox*>("preference_language");language->setCurrentIndex(language->findData("ru"));
                stage=1;dialog->findChild<QPushButton*>("preferencesPreview")->click();
            } else if(stage==1) {
                QCOMPARE(LocalizationService::instance().language(),QString("ru"));
                QCOMPARE(dialog->windowTitle(),QString::fromUtf8("Настройки"));
                QVERIFY(LocalizationService::instance().qtTranslationLoaded());stage=2;dialog->reject();
            }
        });timer.start(30);showPreferences(window);timer.stop();QCOMPARE(stage,2);
        QCOMPARE(PreferencesService::instance().current().toJson(),preferences);QCOMPARE(sceneToJson(window.document().scene),document);
        QCOMPARE(window.document().undo.count(),undo);QCOMPARE(window.document().frame,frame);
        QStringList after;for(auto *action:ActionRegistry(window).actions())after<<ActionRegistry::id(action);after.sort();before.sort();QCOMPARE(after,before);
        window.document().undo.setClean();window.close();
    }
};
#include "atom_test_main.h"
int main(int argc,char **argv) {return runJesterTests<VisualTests>(argc,argv,"JesterVisualTests");}
#include "visual_tests.moc"
