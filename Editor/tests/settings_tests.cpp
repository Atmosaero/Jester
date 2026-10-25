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
