#include "io/project/project_io.h"
#include "io/project/recovery.h"
#include "io/catalog/demo_scenes.h"
#include "core/annotations/motion_annotations.h"
#include "core/analysis/analysis_jobs.h"
#include "physics/physics_provenance.h"
#include "core/scene/visual_settings.h"
#include "settings/preferences.h"
#include "ui/curves/curve_canvas.h"
#include <QApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>
using namespace trickster;
class SettingsTests : public QObject {
    Q_OBJECT
  private slots:
    void visualEditsDoNotInvalidateMotionOrPhysics() {
        Workspace workspace;
        auto &document=workspace.active();document.replace(Scene::mannequin());
        AnalysisJobs jobs(workspace);
        const auto id=jobs.start(document,"trajectory",{{"nodes",QJsonArray{0}},{"start",0},{"end",.2},{"step",.1}})["id"].toString();
        QTRY_COMPARE_WITH_TIMEOUT((jobs.poll(),jobs.state(id)["status"].toString()),QString("ready"),10000);
        const auto original=sceneToJson(document.scene);
        const auto motion=document.motionRevision,metadata=document.metadataRevision,pose=document.poseRevision,mesh=document.meshRevision;
        PhysicsReport before,after;
        recordPhysicsProvenance(document.scene,0,30,before,{});
        auto settings=document.scene.visuals;QString error;
        QVERIFY(settings.setValue("lighting.main.intensityLux",2700,error));
        QVERIFY(settings.setValue("post.bloom.enabled",true,error));
        QVERIFY(document.editVisual("Scene lighting",settings));
        QVERIFY(document.dirty());
        QCOMPARE(document.motionRevision,motion);QCOMPARE(document.metadataRevision,metadata);
        QCOMPARE(document.poseRevision,pose);QCOMPARE(document.meshRevision,mesh);
        QCOMPARE(jobs.state(id)["status"].toString(),QString("ready"));
        recordPhysicsProvenance(document.scene,0,30,after,{});
        QCOMPARE(after.details["inputSha256"],before.details["inputSha256"]);
        const auto changed=sceneToJson(document.scene);
        for(const auto &field:{"keys","curves","layers","contacts","annotations","nodes","meshes","physics"})
            QCOMPARE(changed[field],original[field]);
        document.undo.undo();QCOMPARE(document.scene.visuals.toJson(),original["visuals"].toObject());
        QCOMPARE(document.motionRevision,motion);QCOMPARE(document.poseRevision,pose);
        document.undo.redo();QCOMPARE(document.scene.visuals.toJson(),settings.toJson());
    }
    void visualGestureIsOneUndoAndSurvivesSessionHistory() {
        Document document;document.replace(Scene::mannequin());
        const auto original=document.scene.visuals;
        auto settings=original;QString error;
        QVERIFY(settings.setValue("post.exposureEV",1,error));
        QVERIFY(document.editVisual("Exposure",settings,71));
        const auto firstRevision=document.revision;
        QVERIFY(settings.setValue("post.exposureEV",2,error));
        QVERIFY(document.editVisual("Exposure",settings,71));
        QCOMPARE(document.undo.count(),1);
        QVERIFY(document.revision>firstRevision);
        const auto keys=sceneToJson(document.scene)["keys"];
        Document restored;restored.restoreSession(document.captureSession());
        QCOMPARE(restored.scene.visuals.toJson(),settings.toJson());
        restored.undo.undo();QCOMPARE(restored.scene.visuals.toJson(),original.toJson());
        QCOMPARE(sceneToJson(restored.scene)["keys"],keys);
        restored.undo.redo();QCOMPARE(restored.scene.visuals.toJson(),settings.toJson());
        document.play(true);QVERIFY(document.playing);
        QVERIFY(settings.setValue("background.mode","gradient",error));
        QVERIFY(document.editVisual("Background",settings));
        QVERIFY(document.playing);document.play(false);
    }
    void visualSettingsPersistAndRejectMalformedProjectState() {
        auto scene=Scene::mannequin();QString error;
        scene.visuals=SceneVisualSettings::preset("studio");
        QVERIFY(scene.visuals.setValue("mode","wireframe",error));
        QVERIFY(scene.visuals.setValue("post.exposureEV",1.25,error));
        const auto json=sceneToJson(scene);QCOMPARE(json["version"].toInt(),9);
        Scene loaded;QVERIFY2(sceneFromJson(json,loaded,error),qPrintable(error));
        QCOMPARE(loaded.visuals.toJson(),scene.visuals.toJson());
        auto invalid=json;invalid["visuals"]=QJsonObject{{"camera",QJsonObject{{"fovDegrees",9999}}}};
        const auto original=sceneToJson(loaded);
        QVERIFY(!sceneFromJson(invalid,loaded,error));QCOMPARE(sceneToJson(loaded),original);
    }
    void jstIsTheOnlyNativeExtension() {
        QString error;
        QVERIFY(isNativeProjectFile("scene.jst"));
        QVERIFY(isNativeProjectFile("SCENE.JST"));
        QVERIFY(!isNativeProjectFile("scene.jst.jst"));
        QVERIFY(!isSupportedSceneFile("scene.trickster"));
        QCOMPARE(normalizedProjectSavePath("scene",error),QString("scene.jst"));
        QCOMPARE(normalizedProjectSavePath("SCENE.JST",error),QString("SCENE.JST"));
        QVERIFY(normalizedProjectSavePath("scene.jst.jst",error).isEmpty());
        QVERIFY(normalizedProjectSavePath("scene.glb",error).isEmpty());
        QCOMPARE(suggestedProjectFileName("Character / retarget.jst"),QString("Character _ retarget.jst"));
        QTemporaryDir directory;
        auto scene=Scene::mannequin();
        const auto oldPath=directory.filePath("unsupported.trickster");
        QVERIFY(!saveProject(oldPath,scene,error));
        QVERIFY(!QFile::exists(oldPath));
        const auto before=sceneToJson(scene);
        QVERIFY(!loadProject(oldPath,scene,error));
        QVERIFY(!loadCharacter(oldPath,scene,error));
        QCOMPARE(sceneToJson(scene),before);
    }
    void jstRoundTripRetainsAuthoredDataAndRecovery() {
        QTemporaryDir directory;
        const auto folder=directory.path()+QString::fromUtf8("/Проекты Jester с пробелами");
        QVERIFY(QDir().mkpath(folder));
        auto scene=mvpDemo("walk");
        editAnnotations(scene,{{"operation","add"},{"annotation",QJsonObject{
            {"name",QString::fromUtf8("Проверить контакт")},{"start",0.0},{"end",.5},
            {"nodes",QJsonArray{0}},{"description",QString::fromUtf8("Заметка к анимации")}}}});
        QString error;
        const auto path=folder+QString::fromUtf8("/Анимация персонажа.JST");
        const auto before=sceneToJson(scene);
        QVERIFY2(saveProject(path,scene,error),qPrintable(error));
        Scene reopened;
        QVERIFY2(loadProject(path,reopened,error),qPrintable(error));
        const auto after=sceneToJson(reopened);
        for(const auto &field:{"keys","curves","layers","contacts","annotations","nodes","meshes"})
            QCOMPARE(after[field],before[field]);
        Workspace workspace;workspace.active().replace(reopened,path);
        workspace.active().edit("Make recovery dirty",[](Scene &s){s.name+=" edited";},false);
        RecoveryStore recovery(workspace,folder+"/recovery");
        QVERIFY2(recovery.flush(error),qPrintable(error));
        const auto snapshots=recovery.snapshots();QCOMPARE(snapshots.size(),1);
        QVERIFY(isNativeProjectFile(snapshots[0]));
        Scene snapshot;
        QVERIFY2(loadProject(snapshots[0],snapshot,error),qPrintable(error));
        QVERIFY2(recovery.recover(snapshots[0],error),qPrintable(error));
        QVERIFY(workspace.active().path.isEmpty());
        QVERIFY(workspace.active().dirty());
        Scene original;
        QVERIFY(loadProject(path,original,error));
        QCOMPARE(sceneToJson(original)["keys"],before["keys"]);
    }
    void visualSettingsAreValidatedAtomically() {
        SceneVisualSettings settings;
        QString error;
        const auto original=settings.toJson();
        QVERIFY(!settings.patch({{"lighting",QJsonObject{{"main",QJsonObject{{"intensityLux",-1}}}}}},error));
        QCOMPARE(settings.toJson(),original);
        QVERIFY(error.startsWith("visual.invalid_value"));
        QVERIFY(!settings.patch({{"lighting",QJsonObject{{"unsupported",true}}}},error));
        QCOMPARE(settings.toJson(),original);
        QVERIFY(!settings.setValue("camera.fovDegrees",std::numeric_limits<double>::infinity(),error));
        QVERIFY(!settings.setValue("mode","skeleton",error));
        QVERIFY(!settings.setValue("post.bloom.enabled",1,error));
        QVERIFY(settings.setValue("mode","unlit",error));
        QVERIFY(settings.setValue("post.exposureEV",1.25,error));
        SceneVisualSettings restored;
        QVERIFY(SceneVisualSettings::fromJson(settings.toJson(),restored,error));
        QCOMPARE(restored.toJson(),settings.toJson());
        for(const auto &name:{QString("neutral"),QString("studio"),QString("silhouette")}) {
            const auto preset=SceneVisualSettings::preset(name);
            QVERIFY2(SceneVisualSettings::fromJson(preset.toJson(),restored,error),qPrintable(error));
        }
    }
    void preferencesRejectSecretsAndConflicts() {
        EditorPreferences preferences;
        const auto original=preferences.toJson();
        QString error;
        QVERIFY(!preferences.patch({{"mcp",QJsonObject{{"token","not-a-real-secret"}}}},error));
        QVERIFY(!preferences.patch({{"recentFiles",QJsonArray{"private/path"}}},error));
        QVERIFY(!preferences.patch({{"shortcuts",QJsonObject{{"saveAction","Ctrl+K"},{"openAction","Ctrl+K"}}}},error));
        QVERIFY(!preferences.patch({{"layouts",QJsonObject{{"Bad","aW52YWxpZC1sYXlvdXQ="}}}},error));
        QVERIFY(!preferences.patch({{"appearance",QJsonObject{{"uiScale",0}}}},error));
        QCOMPARE(preferences.toJson(),original);
        QVERIFY(preferences.patch({{"language","ru"},{"appearance",QJsonObject{{"theme","high-contrast"},{"fontSize",13},{"uiScale",1.5}}}},error));
        auto invalid=original;invalid["version"]=2;
        EditorPreferences target=preferences;
        QVERIFY(!EditorPreferences::fromJson(invalid,target,error));
        QCOMPARE(target.toJson(),preferences.toJson());
        QVERIFY(!EditorPreferences::fromJson({},target,error));
    }
    void preferencesRoundTripThroughUnicodePath() {
        QTemporaryDir folder;
        const auto directory=folder.path()+QString::fromUtf8("/Настройки редактора");
        QVERIFY(QDir().mkpath(directory));
        EditorPreferences preferences;
        QString error;
        QVERIFY(preferences.patch({{"language","ru"},{"appearance",QJsonObject{{"accent","#dfac52"}}},
            {"shortcuts",QJsonObject{{"saveAction","Ctrl+Alt+S"}}}},error));
        const auto path=directory+"/preferences.json";
        QVERIFY2(preferences.save(path,error),qPrintable(error));
        EditorPreferences restored;
        QVERIFY2(EditorPreferences::load(path,restored,error),qPrintable(error));
        QCOMPARE(restored.toJson(),preferences.toJson());
    }
    void quaternionBoxUsesVisibleKeyPosition() {
        Document document;
        auto scene=Scene::mannequin();
        ScalarCurve curve;curve.id="q";curve.path="rotationQuaternion";curve.node=0;
        for(int i=0;i<3;++i) {
            CurveKey key;key.time=i;key.value=0;key.orientation=QQuaternion::fromAxisAndAngle(0,1,0,float(45+45*i));
            curve.keys.push_back(key);
        }
        scene.curves={curve};scene.curveBasePose=scene.pose;
        document.replace(scene);
        CurveCanvas canvas(&document);canvas.id="q";canvas.resize(640,360);canvas.fit();canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const auto point=canvas.point(1,sampleCurve(curve,1)).toPoint();
        QVERIFY(canvas.rect().contains(point));
        // Click, rectangle and rendering must agree on quaternion angle coordinates.
        QTest::mouseClick(&canvas,Qt::LeftButton,Qt::ControlModifier,point);
        QVERIFY(canvas.selected.contains(1));
        canvas.selected.clear();
        QTest::mousePress(&canvas,Qt::LeftButton,Qt::ShiftModifier,point-QPoint(12,12));
        QTest::mouseMove(&canvas,point+QPoint(12,12));
        QTest::mouseRelease(&canvas,Qt::LeftButton,Qt::ShiftModifier,point+QPoint(12,12));
        QCOMPARE(canvas.selected,QSet<int>{1});
        QCOMPARE(document.scene.curves[0].keys[1].orientation,curve.keys[1].orientation);
    }
};
QTEST_MAIN(SettingsTests)
#include "settings_tests.moc"
