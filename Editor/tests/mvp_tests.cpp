#include "io/project/project_io.h"
#include "io/catalog/demo_scenes.h"
#include "core/annotations/motion_annotations.h"
#include "core/analysis/motion_analysis.h"
#include "core/analysis/analysis_jobs.h"
#include "core/animation/retarget.h"
#include "core/scene/edit_service.h"
#include "physics/physics_jobs.h"
#include "io/project/recovery.h"
#include "core/scene/workspace.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdlib>
using namespace trickster;
class MVPTests : public QObject {
    Q_OBJECT
  private slots:
    void partialRetargetKeepsContactProvenanceAndValidity() {
        Scene source=Scene::mannequin(), target=source;
        source.last=3;
        source.keys[0]={source.pose,{},source.morphs};
        source.keys[3]=source.keys[0];
        editAnnotations(source,{{"operation","add"},{"annotation",QJsonObject{
            {"id","allowed-pair"},{"name","Authored contact"},{"type","allowed-contact"},
            {"start",0.0},{"end",.1},{"nodes",QJsonArray{0,4}},
            {"description","These two bones may touch"},{"provenance","intention"}}}});
        const auto original=source.annotations;
        QJsonObject report;
        auto result=retargetMotion(source,target,{{"fps",30},{"ik",false},{"contacts",false},
            {"mapping",QJsonArray{QJsonObject{{"role","pelvis"},{"source",0},{"target",0}}}}},report);
        QString error;
        QVERIFY2(result.validate(&error),qPrintable(error));
        QCOMPARE(source.annotations,original);
        auto annotation=result.annotations[0].toObject();
        QVERIFY(annotation["type"]!="allowed-contact");
        QCOMPARE(annotation["sourceAnnotation"].toObject(),original[0].toObject());
        QCOMPARE(annotation["transferStatus"].toString(),QString("incomplete-mapping"));
        QVERIFY(!report["warnings"].toArray().isEmpty());
        QTemporaryDir folder;
        QVERIFY2(saveProject(folder.filePath("retarget.jst"),result,error),qPrintable(error));
        Scene reopened;
        QVERIFY2(loadProject(folder.filePath("retarget.jst"),reopened,error),qPrintable(error));
        QCOMPARE(reopened.annotations,result.annotations);
        // The validator must continue rejecting actual one-ended allowed contacts.
        auto invalid=source;
        auto contact=invalid.annotations[0].toObject();contact["nodes"]=QJsonArray{0};
        invalid.annotations[0]=contact;
        QVERIFY(!invalid.validate(&error));
    }
    void expectationProblemPeakUsesMeasuredError() {
        Workspace workspace;
        auto scene=Scene::mannequin();
        scene.first=0;scene.last=30;scene.physics.metersPerUnit=.01f;
        scene.curveBasePose=scene.pose;
        scene.curves={{"deviation","position",0,0,{{0,2},{.5,5},{1,3}}}};
        for(auto &key:scene.curves[0].keys)key.interpolation="Linear";
        const auto origin=scene.positions(scene.pose)[0]*scene.physics.metersPerUnit;
        editAnnotations(scene,{{"operation","add"},{"annotation",QJsonObject{
            {"name","Position intent"},{"start",0.0},{"end",1.01},{"nodes",QJsonArray{0}},
            {"expectations",QJsonArray{QJsonObject{{"kind","position"},{"space","world"},
                {"units","metres"},{"target",QJsonArray{0,origin.y(),origin.z()}},{"tolerance",.001}}}}}}});
        workspace.active().replace(scene);
        AnalysisJobs jobs(workspace);
        auto id=jobs.start(workspace.active(),"validate",{{"start",0},{"end",1},{"step",.25}})["id"].toString();
        QTRY_COMPARE_WITH_TIMEOUT((jobs.poll(),jobs.state(id)["status"].toString()),QString("ready"),10000);
        QJsonObject problem;
        for(auto value:jobs.result(id,0,100)["problems"].toArray())
            if(value.toObject()["kind"]=="expectation-fail")problem=value.toObject();
        QVERIFY(!problem.isEmpty());
        QCOMPARE(problem["peakSeconds"].toDouble(),.5);
        QVERIFY(std::abs(problem["measurement"].toObject()["error"].toDouble()-.05)<1e-5);
        QCOMPARE(problem["measurement"].toObject()["units"].toString(),QString("metres"));
    }
    void undefinedExpectationHasNoInventedPeak() {
        Workspace workspace;
        auto scene=Scene::mannequin();scene.last=30;
        editAnnotations(scene,{{"operation","add"},{"annotation",QJsonObject{
            {"name","Unmeasurable orientation"},{"start",0.0},{"end",1.01},{"nodes",QJsonArray{0}},
            {"expectations",QJsonArray{QJsonObject{{"kind","orientation"},{"space","world"},
                {"units","degrees"},{"target",QJsonArray{0,0,0,0}},{"tolerance",1}}}}}}});
        workspace.active().replace(scene);
        AnalysisJobs jobs(workspace);
        auto id=jobs.start(workspace.active(),"validate",{{"start",0},{"end",1},{"step",.25}})["id"].toString();
        QTRY_COMPARE_WITH_TIMEOUT((jobs.poll(),jobs.state(id)["status"].toString()),QString("ready"),10000);
        QJsonObject problem;
        for(auto value:jobs.result(id,0,100)["problems"].toArray())
            if(value.toObject()["kind"]=="expectation-undefined")problem=value.toObject();
        QVERIFY(!problem.isEmpty());
        QVERIFY(problem["peakSeconds"].isNull());
        QVERIFY(problem["peakValue"].isNull());
        QCOMPARE(problem["status"].toString(),QString("uncertain"));
    }
    void poseEditingOverAuthoredCurves() {
        Document d;d.replace(Scene::mannequin());ScalarCurve source;source.id="source";source.node=0;source.path="position";source.keys={{0,0},{1,1}};source.keys[0].interpolation="Linear";
        EditService::execute(d,"animation_curves",{{"operation","set"},{"curve",curvesJson({source})[0]}});d.seek(15);auto before=d.scene.pose;
        int count=d.undo.count();auto head=before[4].t+QVector3D(.1f,0,0);
        EditService::execute(d,"pose_transform",{{"node",4},{"position",QJsonArray{head.x(),head.y(),head.z()}}});
        QCOMPARE(d.undo.count(),count+1);QCOMPARE(d.scene.layers.size(),size_t(1));d.seek(10);d.seek(15);QVERIFY((d.scene.pose[4].t-head).length()<1e-5);QCOMPARE(d.scene.pose[0].t.x(),.5f);
        d.scene.compareOriginal=true;d.scene.evaluate(15);QVERIFY((d.scene.pose[4].t-before[4].t).length()<1e-5);d.scene.compareOriginal=false;
        d.undo.undo();QVERIFY(d.scene.layers.empty());d.seek(15);QVERIFY((d.scene.pose[4].t-before[4].t).length()<1e-5);
        // The real Document gesture uses the same layer policy as the typed service.
        d.begin();d.scene.pose[4].t=head;d.commit("Gesture over curve");d.seek(10);d.seek(15);QVERIFY((d.scene.pose[4].t-head).length()<1e-5);
        ScalarCurve layerCurve;layerCurve.id="layer";layerCurve.layer=d.scene.correction().id;layerCurve.node=4;layerCurve.path="position";layerCurve.keys={{0,.1},{1,.1}};layerCurve.keys[0].interpolation="Linear";
        EditService::execute(d,"animation_curves",{{"operation","set"},{"curve",curvesJson({layerCurve})[0]}});d.seek(15);auto goal=d.scene.pose[4].t+QVector3D(.15f,0,0);
        EditService::execute(d,"pose_transform",{{"node",4},{"position",QJsonArray{goal.x(),goal.y(),goal.z()}}});QCOMPARE(d.scene.layers.size(),size_t(2));d.seek(10);d.seek(15);QVERIFY((d.scene.pose[4].t-goal).length()<1e-5);d.undo.undo();QCOMPARE(d.scene.layers.size(),size_t(1));
        Document ik;ik.replace(Scene::mannequin());auto positions=ik.scene.positions(ik.scene.pose);ScalarCurve controller;controller.id="ik-curve";controller.node=8;controller.path="ikTarget";controller.component=1;controller.keys={{0,positions[8].y()},{1,positions[8].y()}};
        EditService::execute(ik,"animation_curves",{{"operation","set"},{"curve",curvesJson({controller})[0]}});ik.seek(15);auto target=ik.scene.positions(ik.scene.pose)[8]+QVector3D(-.08f,-.06f,0);
        EditService::execute(ik,"pose_ik",{{"node",8},{"target",QJsonArray{target.x(),target.y(),target.z()}}});ik.seek(10);ik.seek(15);QVERIFY((ik.scene.positions(ik.scene.pose)[8]-target).length()<.001);
    }
    void retargetContextRevisionGuard() {
        Workspace workspace;workspace.active().replace(Scene::mannequin());auto *source=&workspace.active();workspace.append(Scene::mannequin(),{},false);AnalysisJobs jobs(workspace);auto &target=workspace.active();
        auto id=jobs.start(target,"retarget",{{"fps",10},{"ik",false}},source)["id"].toString();
        QTRY_COMPARE_WITH_TIMEOUT((jobs.poll(),jobs.state(id)["status"].toString()),QString("ready"),10000);
        EditService::execute(*source,"motion_annotations",{{"operation","add"},{"annotation",QJsonObject{{"name","Changed source intention"},{"start",0},{"end",1}}}});
        QCOMPARE(jobs.state(id)["status"].toString(),QString("stale"));QVERIFY_EXCEPTION_THROWN(jobs.resultScene(id,target),std::exception);
    }
    void assistedSnapshotParity() {
        Scene s=Scene::mannequin();s.bodyAssist=true;s.pins[8]={{2.5f,2.4f,.2f},s.defaultPole(8),1};auto snapshot=s.motionOnlyCopy();
        s.evaluate(0);snapshot.evaluate(0);auto expected=s.positions(s.pose),actual=snapshot.positions(snapshot.pose);
        for(size_t n=0;n<expected.size();++n)QVERIFY((expected[n]-actual[n]).length()<1e-6);
    }
    void retargetSoleIntentions() {
        Scene source=Scene::mannequin();source.last=6;source.keys[0]={source.pose,{},source.morphs};source.keys[6]=source.keys[0];
        for(int n:{15,19})source.contacts.push_back({n,0,6,source.positions(source.pose)[n],source.defaultPole(n)});
        auto target=source;target.contacts.clear();target.keys.clear();QJsonObject report;auto before=sceneToJson(source);
        auto result=retargetMotion(source,target,{{"fps",30},{"contacts",true},{"ik",true}},report);
        QCOMPARE(sceneToJson(source),before);QVERIFY(std::abs(report["bodyScale"].toDouble()-1)<1e-6);
        QJsonObject workerReport;retargetMotion(source.motionOnlyCopy(),target,{{"fps",30},{"contacts",true}},workerReport);QCOMPARE(workerReport["bodyScale"],report["bodyScale"]);
        QCOMPARE(report["transferredContactIntentions"].toInt(),2);QVERIFY(report["maximumFootGoalResidualSceneUnits"].toDouble()<.001);
        auto measured=measureExpectations(result,.1,1./30);QVERIFY(measured.size()>=2);for(auto value:measured)QCOMPARE(value.toObject()["status"].toString(),QString("pass"));
        auto saved=sceneToJson(result);Scene reopened;QString error;QVERIFY2(sceneFromJson(saved,reopened,error),qPrintable(error));QCOMPARE(reopened.annotations,result.annotations);
    }
    void bulkTimelineInterpolation() {
        Document d;auto s=Scene::mannequin();s.keys[0]={s.pose,{},s.morphs};s.pose[0].t.setX(1);s.keys[30]={s.pose,{},s.morphs};d.replace(s);
        QJsonValue keys=sceneToJson(d.scene)["keys"];auto before=d.undo.count();
        EditService::execute(d,"animation_curves",{{"operation","interpolation"},{"selection",QJsonArray{QJsonObject{{"frame",0},{"node",0},{"channels",1}}}},{"interpolation","Step"}});
        QCOMPARE(d.undo.count(),before+1);QCOMPARE(d.scene.curves.size(),size_t(3));d.scene.evaluate(29);QCOMPARE(d.scene.pose[0].t.x(),0.f);d.scene.evaluate(30);QCOMPARE(d.scene.pose[0].t.x(),1.f);QCOMPARE(sceneToJson(d.scene)["keys"],keys);
        d.undo.undo();QVERIFY(d.scene.curves.empty());d.scene.evaluate(15);QCOMPARE(d.scene.pose[0].t.x(),.5f);d.undo.redo();d.scene.evaluate(15);QCOMPARE(d.scene.pose[0].t.x(),0.f);
    }
    void nativeCurveExtractionAndSparseCache() {
        Scene source=Scene::mannequin();AnimationChannel native;native.node=0;native.path="translation";native.components=3;
        native.interpolation="CUBICSPLINE";native.times={0,.7f,2};
        native.values={0,0,0, 0,1,0, 2,0,0, 1,0,0, 1,1,0, -1,0,0, -2,0,0, 0,1,0, 0,0,0};
        AnimationClip clip;clip.channels={native};clip.duration=2;source.clips=std::make_shared<const std::vector<AnimationClip>>(std::vector<AnimationClip>{clip});source.sourceClip=0;
        auto curve=extractNativeCurve(source,0,0);
        for(int i=0;i<=200;++i){double t=i*.01;source.evaluateNativeSource(float(t*source.fps));QVERIFY(std::abs(source.pose[0].t.x()-sampleCurve(curve,t))<2e-6);}
        Document document;document.replace(source);QJsonValue original=sceneToJson(document.scene)["clips"];
        EditService::execute(document,"animation_curves",{{"operation","extract_native"},{"channel",0},{"component",0}});
        QCOMPARE(document.scene.curves.size(),size_t(1));QCOMPARE(sceneToJson(document.scene)["clips"],original);document.undo.undo();QVERIFY(document.scene.curves.empty());
        MotionCorrection layer;Transform a,b;a.t={0,0,0};a.s={0,0,0};b=a;b.t={2,0,0};layer.keys[0].offsets[1]=a;layer.keys[60].offsets[1]=b;
        QCOMPARE(source.sampleLayer(layer,30).offsets[1].t.x(),1.f);
        auto copy=layer;copy.keys[60].offsets[1].t={4,0,0};QCOMPARE(source.sampleLayer(copy,30).offsets[1].t.x(),2.f);QCOMPARE(source.sampleLayer(layer,30).offsets[1].t.x(),1.f);
        auto grid=validationSampleTimes(source,{{"start",0},{"end",2},{"step",1},{"adaptiveDepth",2}});QVERIFY(grid.size()>3);QVERIFY(grid.contains(.7f));
    }
    void comparisonMetricsAndLag() {
        Scene left=Scene::mannequin();left.last=30;left.curveBasePose=left.pose;left.curves={{"x","position",0,0,{{0,0},{1,1}}}};left.curves[0].keys[0].interpolation="Linear";Scene right=left;right.curves[0].keys[0].value=-.1;right.curves[0].keys[1].value=.9;
        auto lag=compareMotion(left,right,{{"operation","lag"},{"minimumLag",-.2},{"maximumLag",.2},{"lagStep",.1},{"budgetMs",1000},{"mapping",QJsonArray{QJsonObject{{"left",0},{"right",0}}}}});QVERIFY(std::abs(lag["bestOffsetSeconds"].toDouble()-.1)<1e-5);
        auto report=compareMotion(left,right,{{"start",.5},{"end",.5},{"offsetSeconds",.1},{"budgetMs",1000},{"mapping",QJsonArray{QJsonObject{{"left",0},{"right",0}}}}});auto bone=report["samples"].toArray()[0].toObject()["bones"].toArray()[0].toObject();QVERIFY(bone["linearVelocityError"].toDouble()<1e-4);QVERIFY(std::abs(report["cycles"].toObject()["left"].toObject()["positionClosureRms"].toDouble()-1)<1e-5);
        ScalarCurve q;q.id="arm";q.node=7;q.path="rotationQuaternion";q.keys={{0,0},{1,0}};q.keys[0].interpolation="Linear";q.keys[1].orientation=QQuaternion::fromEulerAngles(0,0,30);left.curves={q};right.curves={q};right.curves[0].keys[0].orientation=QQuaternion::fromEulerAngles(0,0,5);right.curves[0].keys[1].orientation=QQuaternion::fromEulerAngles(0,0,35);auto anatomy=compareMotion(left,right,{{"start",.5},{"end",.5},{"mapping",QJsonArray{QJsonObject{{"left",7},{"right",7},{"axisLeft",QJsonArray{0,0,1}},{"axisRight",QJsonArray{0,0,1}}}}}})["samples"].toArray()[0].toObject()["bones"].toArray()[0].toObject();QVERIFY(std::abs(anatomy["twistErrorDegrees"].toDouble()-5)<.001);QVERIFY(anatomy["swingErrorDegrees"].toDouble()<.001);
    }
    void attachedContextTimePolicies() {
        Document d;d.replace(Scene::mannequin());d.scene.keys[0]={d.scene.pose,{},d.scene.morphs};d.scene.keys[30]=d.scene.keys[0];auto id=EditService::execute(d,"motion_annotations",{{"operation","add"},{"annotation",QJsonObject{{"name","Key intent"},{"start",1},{"end",1.2}}}})["annotation"].toObject()["id"].toString();EditService::execute(d,"motion_annotations",{{"operation","attach"},{"id",id},{"frame",30}});auto keyId=d.scene.keys[30].id;QVERIFY(!keyId.isEmpty());EditService::execute(d,"timeline",{{"operation","move"},{"from",30},{"frame",45}});QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),1.5);d.undo.undo();QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),1.);
        EditService::execute(d,"timeline",{{"operation","range"},{"first",0},{"last",90},{"fps",60}});QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),1.);EditService::execute(d,"timeline",{{"operation","move"},{"from",30},{"frame",60}});QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),1.5);
        QTemporaryDir temp;QString error;QVERIFY(saveProject(temp.path()+"/attached.jst",d.scene,error));Scene reopened;QVERIFY(loadProject(temp.path()+"/attached.jst",reopened,error));QCOMPARE(reopened.keys[60].id,keyId);QCOMPARE(reopened.annotations,d.scene.annotations);
        EditService::execute(d,"timeline",{{"operation","delete"},{"frame",60}});QCOMPARE(d.scene.annotations[0].toObject()["attachment"].toObject()["status"].toString(),QString("orphaned-retained"));EditService::execute(d,"motion_annotations",{{"operation","retime"},{"id",id},{"scale",2},{"pivot",1}});QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),2.);EditService::execute(d,"motion_annotations",{{"operation","trim"},{"start",0},{"end",.5}});QCOMPARE(d.scene.annotations.size(),1);QCOMPARE(d.scene.annotations[0].toObject()["trimStatus"].toString(),QString("outside-retained"));
        auto profileRevision=d.profileRevision;EditService::execute(d,"character_profile",{{"operation","suggest"}});QVERIFY(d.profileRevision>profileRevision);auto limitRevision=d.limitsRevision;EditService::execute(d,"pose_limits",{{"node",7},{"enabled",true},{"kind","hinge"},{"axis",QJsonArray{0,0,1}},{"minimum",-5},{"maximum",90}});QVERIFY(d.limitsRevision>limitRevision);
    }
    void layerAndControllerCurves() {
        Document d;d.replace(Scene::mannequin());QString error;QVERIFY(d.scene.beginCorrection(0,60,0,0,false,error));auto layer=d.scene.correction().id;ScalarCurve curve;curve.id="layer-x";curve.layer=layer;curve.node=0;curve.path="position";curve.keys={{0,0},{1,.2}};curve.keys[0].interpolation="Linear";EditService::execute(d,"animation_curves",{{"operation","set"},{"id",curve.id},{"curve",curvesJson({curve})[0]}});d.scene.evaluate(15);QVERIFY(std::abs(d.scene.pose[0].t.x()-.1)<1e-5);d.scene.correction().enabled=false;d.scene.evaluate(15);QVERIFY(std::abs(d.scene.pose[0].t.x())<1e-6);d.scene.correction().enabled=true;
        QTemporaryDir temp;QVERIFY(saveProject(temp.path()+"/layer.jst",d.scene,error));Scene reopened;QVERIFY(loadProject(temp.path()+"/layer.jst",reopened,error));QCOMPARE(reopened.curves[0].layer,layer);reopened.evaluate(15);QVERIFY(std::abs(reopened.pose[0].t.x()-.1)<1e-5);
        Document controller;controller.replace(Scene::mannequin());auto target=controller.scene.positions(controller.scene.pose)[8];ScalarCurve hand;hand.id="target-y";hand.node=8;hand.path="ikTarget";hand.component=1;hand.keys={{0,target.y()},{1,target.y()+.1}};hand.keys[0].interpolation="Linear";EditService::execute(controller,"animation_curves",{{"operation","set"},{"id",hand.id},{"curve",curvesJson({hand})[0]}});controller.scene.evaluate(15);QVERIFY(controller.scene.pins.contains(8));QVERIFY(std::abs(controller.scene.pins[8].target.y()-target.y()-.05)<1e-5);EditService::execute(controller,"animation_curves",{{"operation","remove"},{"id",hand.id}});QVERIFY(controller.scene.pins.empty());
    }
    void authoredExpectations() {
        Scene s=Scene::mannequin();s.curveBasePose=s.pose;s.curves={{"x","position",0,0,{{0,0},{1,2}}}};s.curves[0].keys[0].interpolation="Linear";auto point=s.pose[0].t;point.setX(1);
        s.annotations=QJsonArray{QJsonObject{{"id","intent"},{"name","Measured intent"},{"start",0},{"end",1},{"nodes",QJsonArray{0}},{"provenance","intention"},{"expectations",QJsonArray{QJsonObject{{"kind","position"},{"space","world"},{"units","metres"},{"target",QJsonArray{1,point.y(),point.z()}},{"tolerance",.001}},QJsonObject{{"kind","speed"},{"space","world"},{"units","m/s"},{"minimum",1.99},{"maximum",2.01},{"tolerance",.001}}}}}};auto before=s.annotations;auto measured=measureExpectations(s,.5,.01);QCOMPARE(measured.size(),2);QCOMPARE(measured[0].toObject()["status"].toString(),QString("pass"));QCOMPARE(measured[1].toObject()["status"].toString(),QString("pass"));auto failed=measureExpectations(s,.25,.01);QCOMPARE(failed[0].toObject()["status"].toString(),QString("fail"));QCOMPARE(s.annotations,before);
    }
    void quaternionAndAdaptiveExport() {
        ScalarCurve q;q.id="orientation";q.path="rotationQuaternion";q.node=0;q.keys={{0,0},{1,0},{3,0}};q.keys[0].orientation=QQuaternion::fromEulerAngles(0,0,0);q.keys[1].orientation=QQuaternion::fromEulerAngles(0,90,0);q.keys[2].orientation=-QQuaternion::fromEulerAngles(0,150,0);q.keys[0].interpolation="Linear";
        auto middle=sampleOrientation(q,.5);QVERIFY(std::abs(std::abs(QQuaternion::dotProduct(middle,QQuaternion::fromEulerAngles(0,45,0)))-1)<1e-5);q.keys[0].interpolation="Bezier";for(int i=0;i<=300;++i){auto r=sampleOrientation(q,i/100.);QVERIFY(std::isfinite(r.lengthSquared()));QVERIFY(std::abs(r.length()-1)<1e-5);}
        Scene scene=Scene::mannequin();scene.curveBasePose=scene.pose;scene.curves={q};scene.last=30;QString error;QVERIFY2(scene.validate(&error),qPrintable(error));QTemporaryDir temp;QVERIFY2(exportAnimation(temp.path()+"/orientation.glb",scene,error),qPrintable(error));Scene loaded;QVERIFY2(loadCharacter(temp.path()+"/orientation.glb",loaded,error),qPrintable(error));for(int i=0;i<=60;++i){scene.evaluate(i*.5f);loaded.evaluate(i*.5f*loaded.fps/scene.fps);double dot=std::abs(QQuaternion::dotProduct(scene.pose[0].r,loaded.pose[0].r));QVERIFY(2*std::acos(std::clamp(dot,0.,1.))*180/3.141592653589793<.15);}
        auto times=animationExportTimes(scene);QVERIFY(times.size()>=31);scene.curves.clear();ScalarCurve spin;spin.id="spin";spin.path="rotationEuler";spin.node=0;spin.component=1;spin.keys={{0,0},{1,1440}};spin.keys[0].interpolation="Linear";scene.curves={spin};auto dense=animationExportTimes(scene);QVERIFY(dense.size()>=49);
        Scene mismatch=Scene::mannequin();QJsonObject report;QVERIFY_EXCEPTION_THROWN(retargetMotion(scene,mismatch,{{"sourceFingerprint","wrong"}},report),std::exception);
    }
    void surfaceValidation() {try {
        Scene s;s.nodes={{"VolumeA",-1,{},true},{"VolumeB",-1,{},true}};s.pose=s.restPose();
        auto cube=[](int node,float radius){Mesh m;m.node=node;for(auto p:std::vector<QVector3D>{{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}){Vertex v;v.p=p*radius;m.vertices.push_back(v);}m.indices={0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};return m;};
        s.meshes={cube(0,1),cube(1,.2f)};QJsonObject request{{"start",0},{"end",0},{"surface",true},{"meshPairs",QJsonArray{QJsonValue(QJsonArray{0,1})}},{"budgetMs",1000}};
        auto find=[](QJsonObject report,QString kind){for(auto row:report["samples"].toArray())for(auto issue:row.toObject()["issues"].toArray())if(issue.toObject()["kind"]==kind)return true;return false;};
        Scene combined=s;auto &left=combined.meshes[0];auto right=combined.meshes[1];unsigned base=unsigned(left.vertices.size());left.vertices.insert(left.vertices.end(),right.vertices.begin(),right.vertices.end());for(auto index:right.indices)left.indices.push_back(index+base);combined.meshes.resize(1);QVERIFY(find(validateMotion(combined,{{"start",0},{"end",0},{"surface",true},{"selfIntersections",true},{"budgetMs",1000}}),"surface-component-containment"));
        QVERIFY(find(validateMotion(s,request),"surface-containment"));s.pose[1].t={.95f,0,0};s.keys[0]={s.pose,{},s.morphs};QVERIFY(find(validateMotion(s,request),"surface-intersection"));s.pose[1].t={3,0,0};s.keys[0]={s.pose,{},s.morphs};QVERIFY(!find(validateMotion(s,request),"surface-intersection"));QVERIFY(!find(validateMotion(s,request),"surface-containment"));s.meshes[0].indices.resize(6);QVERIFY(find(validateMotion(s,request),"surface-uncertain"));
        Scene limit=Scene::mannequin();limit.jointLimits[7]={true,{0,0,1},0,-5,5};limit.pose[7].r=QQuaternion::fromEulerAngles(0,0,70);limit.keys[0]={limit.pose,{},limit.morphs};QVERIFY(find(validateMotion(limit,{{"start",0},{"end",0}}),"joint-limit"));
    }catch(const std::exception &e){QFAIL(e.what());}}
    void snapshotAnalysisJobs() {
        Workspace w;w.append(Scene::mannequin());auto &d=w.active();AnalysisJobs jobs(w);
        auto state=jobs.start(d,"trajectory",{{"nodes",QJsonArray{0,4}},{"start",0},{"end",1},{"step",.05}});auto id=state["id"].toString();
        QTRY_VERIFY_WITH_TIMEOUT((jobs.poll(),jobs.state(id)["done"].toBool()),10000);jobs.poll();QCOMPARE(jobs.state(id)["status"].toString(),QString("ready"));auto page=jobs.result(id,0,3);QCOMPARE(page["samples"].toArray().size(),3);QCOMPARE(page["nextResultOffset"].toInt(),3);
        EditService::execute(d,"motion_annotations",{{"operation","add"},{"annotation",QJsonObject{{"name","Metadata"},{"start",0},{"end",1}}}});QCOMPARE(jobs.state(id)["status"].toString(),QString("ready"));
        EditService::execute(d,"pose_transform",{{"node",0},{"position",QJsonArray{1,1,0}}});QCOMPARE(jobs.state(id)["status"].toString(),QString("stale"));QVERIFY_EXCEPTION_THROWN(jobs.result(id,0,3),std::exception);
        auto cancelled=jobs.start(d,"trajectory",{{"nodes",QJsonArray{0}},{"start",0},{"end",90},{"step",.001}})["id"].toString();jobs.cancel(cancelled);QTRY_VERIFY_WITH_TIMEOUT((jobs.poll(),jobs.state(cancelled)["done"].toBool()),10000);jobs.poll();QCOMPARE(jobs.state(cancelled)["status"].toString(),QString("cancelled"));
        Scene a=Scene::mannequin(),b=a;auto report=compareMotion(a,b,{{"start",0},{"end",1},{"step",.1},{"budgetMs",1000}});QVERIFY(report["rmsPosition"].toDouble()<1e-6);QVERIFY(report["rmsOrientationDegrees"].toDouble()<.1);
        b.pose[0].t+=QVector3D(2,0,0);b.keys[0]={b.pose,{},b.morphs};auto translated=compareMotion(a,b,{{"start",0},{"end",0},{"mapping",QJsonArray{QJsonObject{{"left",0},{"right",0}}}}});QVERIFY(std::abs(translated["rmsPosition"].toDouble()-2)<1e-5);auto normalized=compareMotion(a,b,{{"start",0},{"end",0},{"normalizeRoot",true},{"mapping",QJsonArray{QJsonObject{{"left",4},{"right",4}}}}});QVERIFY(normalized["rmsPosition"].toDouble()<1e-5);
    }
    void calibratedRetarget() {
        Scene source=Scene::mannequin();source.keys[0]={source.pose,{},source.morphs};source.translateWorld(0,{1,0,0});source.keys[30]={source.pose,{},source.morphs};source.last=30;Scene target=Scene::mannequin();target.profile=target.suggestedProfile();target.profile.configured=true;target.meshes.clear();for(auto &n:target.nodes){n.rest.t*=1.25;n.name="Other_"+n.name;}target.pose=target.restPose();auto snapshot=source.keys;QJsonObject report;Scene result;try{result=retargetMotion(source,target,{{"fps",30},{"ik",false},{"rootMotion","inPlace"}},report);}catch(const std::exception &e){QFAIL(e.what());}QCOMPARE(result.keys.size(),31);QCOMPARE(source.keys.size(),snapshot.size());QCOMPARE(result.nodes.size(),target.nodes.size());for(auto &k:result.keys)for(size_t n=1;n<result.nodes.size();++n)if(result.nodes[n].name.contains("Foot"))QCOMPARE(k.pose[n].t,target.nodes[n].rest.t);QVERIFY(result.curves.empty());QCOMPARE(report["status"].toString(),QString("approximate"));
    }
    void sampledTrajectory() {
        auto cached=motionTrajectory(Scene::mannequin(),{{"nodes",QJsonArray{0,4,15,19}},{"start",0},{"end",2},{"step",.01},{"limit",300},{"budgetMs",1000}});
        QCOMPARE(cached["samples"].toArray().size(),201);

        Scene s=Scene::mannequin();s.curves={{"x","position",0,0,{{0,0},{2,4}}}};s.curves[0].keys[0].interpolation="Linear";s.curveBasePose=s.pose;
        auto leaf=s;leaf.curves[0].node=8;leaf.curves[0].keys[0].interpolation="Step";
        auto unrelated=motionTrajectory(leaf,{{"nodes",QJsonArray{4}},{"start",1.75},{"end",2},{"step",.25}});
        QVERIFY(!unrelated["samples"].toArray().last().toObject()["bones"].toArray()[0].toObject()["speed"].isNull());
        auto instant=motionTrajectory(s,{{"nodes",QJsonArray{0}},{"start",.5},{"end",.5},{"step",.1}});
        QVERIFY(std::abs(instant["samples"].toArray()[0].toObject()["bones"].toArray()[0].toObject()["velocity"].toArray()[0].toDouble()-2)<1e-4);
        auto before=s.pose;auto frame=s.sampleFrame;
        auto report=motionTrajectory(s,{{"nodes",QJsonArray{0}},{"start",0},{"end",2},{"step",.25},{"limit",20},{"budgetMs",1000}});QCOMPARE(report["status"].toString(),QString("complete"));
        for(auto row:report["samples"].toArray()){auto r=row.toObject();auto b=r["bones"].toArray()[0].toObject();QVERIFY(std::abs(b["position"].toArray()[0].toDouble()-2*r["seconds"].toDouble())<1e-5);QVERIFY(std::abs(b["velocity"].toArray()[0].toDouble()-2)<1e-5);}
        QCOMPARE(s.sampleFrame,frame);QCOMPARE(s.pose[0].t,before[0].t);s.curves[0].keys[0].interpolation="Step";auto jump=motionTrajectory(s,{{"nodes",QJsonArray{0}},{"start",1.75},{"end",2},{"step",.25}});QCOMPARE(jump["samples"].toArray().last().toObject()["bones"].toArray()[0].toObject()["derivativeStatus"].toString(),QString("undefined-at-discontinuity"));
    }
    void editableCurveMath() {
        ScalarCurve c;c.id="curve";c.node=0;c.keys={{0,0},{2,4},{5,1}};
        c.keys[0].interpolation="Step";QCOMPARE(sampleCurve(c,1.999),0.);QCOMPARE(sampleCurve(c,2.),4.);
        c.keys[0].interpolation="Linear";QCOMPARE(sampleCurve(c,.5),1.);
        c.keys[0].interpolation="Hermite";c.keys[0].handles="Free";c.keys[1].handles="Free";c.keys[0].outValue=0;c.keys[1].inValue=0;QCOMPARE(sampleCurve(c,1.),2.);
        c.keys[0].interpolation="Bezier";c.keys[0].outTime=.2;c.keys[0].outValue=.4;c.keys[1].inTime=-.3;c.keys[1].inValue=-.6;QVERIFY(std::abs(sampleCurve(c,.6)-1.2)<1e-10);
        c.keys[0].interpolation="Back";QVERIFY(sampleCurve(c,1.5)>4.);QCOMPARE(sampleCurve(c,0.),0.);QCOMPARE(sampleCurve(c,2.),4.);
        Document d;d.replace(Scene::mannequin());auto result=EditService::execute(d,"animation_curves",{{"operation","set"},{"curve",curvesJson({c})[0]}});QVERIFY(!result["id"].toString().isEmpty());
        d.scene.evaluate(30);QVERIFY(d.scene.pose[0].t.x()>4.);d.undo.undo();QVERIFY(d.scene.curves.empty());d.undo.redo();QCOMPARE(d.scene.curves.size(),size_t(1));
        QTemporaryDir tmp;QString error;QVERIFY(saveProject(tmp.path()+"/curve.jst",d.scene,error));Scene loaded;QVERIFY(loadProject(tmp.path()+"/curve.jst",loaded,error));QCOMPARE(curvesJson(loaded.curves),curvesJson(d.scene.curves));
        auto baked=d.scene.resampledCopy(60);QVERIFY(baked.curves.empty());for(int f=0;f<=90;++f){d.scene.evaluate(float(f));baked.evaluate(float(f)*2);QVERIFY((d.scene.pose[0].t-baked.pose[0].t).length()<1e-5);}
        EditService::execute(d,"animation_curves",{{"operation","remove"},{"id",result["id"]}});QCOMPARE(d.scene.pose[0].t,d.scene.curveBasePose[0].t);
        EditService::execute(d,"pose_transform",{{"node",0},{"position",QJsonArray{3,1,0}},{"key",false}});d.scene.evaluate(30);QCOMPARE(d.scene.pose[0].t.x(),3.f);

    }
    void contextAnnotations() {
        Document d;d.replace(Scene::mannequin());d.seek(12);d.select(3);auto pose=d.scene.pose;
        auto motionRevision=d.motionRevision;
        auto result=EditService::execute(d,"motion_annotations",{{"operation","add"},{"annotation",QJsonObject{{"name","Left support"},{"start",.25},{"end",1.25},{"description","Authored intention"}}}});
        QCOMPARE(d.motionRevision,motionRevision);QVERIFY(d.metadataRevision>0);
        auto id=result["annotation"].toObject()["id"].toString();QVERIFY(!id.isEmpty());QCOMPARE(d.scene.annotations.size(),1);
        QCOMPARE(motionContext(d.scene,.25)["active"].toArray().size(),1);QCOMPARE(motionContext(d.scene,1.25)["active"].toArray().size(),0);
        QCOMPARE(d.frame,12);QCOMPARE(d.selected,3);QCOMPARE(d.scene.pose[0].t,pose[0].t);
        d.undo.undo();QVERIFY(d.scene.annotations.empty());d.undo.redo();QCOMPARE(d.scene.annotations.size(),1);
        EditService::execute(d,"timeline",{{"operation","range"},{"first",0},{"last",90},{"fps",60}});QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),.25);
        QTemporaryDir dir;QString error;QVERIFY(saveProject(dir.path()+"/context.jst",d.scene,error));Scene loaded;QVERIFY(loadProject(dir.path()+"/context.jst",loaded,error));QCOMPARE(loaded.annotations,d.scene.annotations);
        QVERIFY_EXCEPTION_THROWN(EditService::execute(d,"motion_annotations",{{"operation","set"},{"id",id},{"annotation",QJsonObject{{"end",-.5}}}}),std::exception);
        auto count=d.undo.count();queryAnnotations(d.scene,{});QCOMPARE(d.undo.count(),count);
    }
    void persistentDocuments() {
        Workspace workspace;
        workspace.append(Scene::mannequin());
        auto *a = &workspace.active();
        a->select(8);
        a->seek(12);
        EditService::execute(*a, "pose_transform", {{"node", 0}, {"position", QJsonArray{0, 1.2, 0}}});
        auto *geometry = a->scene.meshes[0].vertices.data();
        auto revision = a->revision;
        workspace.append(Scene::mannequin(), {}, false);
        auto *b = &workspace.active();
        QVERIFY(a != b);
        QVERIFY(a->id != b->id);
        QVERIFY(a->dirty());
        QVERIFY(!b->dirty());
        b->select(12);
        b->seek(30);
        for (int i = 0; i < 20; ++i) {
            workspace.activate(0);
            QCOMPARE(&workspace.active(), a);
            workspace.activate(1);
        }
        QCOMPARE(a->scene.meshes[0].vertices.data(), geometry);
        QCOMPARE(a->revision, revision);
        QCOMPARE(a->frame, 12);
        QCOMPARE(a->selected, 8);
        QCOMPARE(b->frame, 30);
        QCOMPARE(b->selected, 12);
        workspace.activate(0);
        workspace.history.undo();
        QVERIFY(!a->dirty());
        QCOMPARE(b->undo.count(), 0);
        workspace.history.redo();
        QVERIFY(a->dirty());
    }
    void keyedLayersCompositionAndIsolation() {
        Document d;
        Scene source = Scene::mannequin();
        source.last = 60;
        source.keys[0] = {source.pose, {}, source.morphs};
        source.keys[60] = source.keys[0];
        d.replace(source);
        QJsonValue sourceJson = sceneToJson(source)["keys"];
        auto create = [&](QString name) {
            EditService::execute(d, "motion_correction",
                                 {{"operation", "create"}, {"name", name}, {"fadeIn", 0}, {"fadeOut", 0}});
        };
        auto lift = [&](int frame, float delta) {
            d.seek(frame);
            auto v = d.scene.pose[8].t + QVector3D(0, delta, 0);
            EditService::execute(d, "pose_transform",
                                 {{"node", 8}, {"position", QJsonArray{v.x(), v.y(), v.z()}}});
        };
        create("Wrist");
        lift(0, .2f);
        lift(30, .2f);
        lift(60, -.4f);
        d.scene.evaluate(15);
        QVERIFY(std::abs(d.scene.pose[8].t.y() - source.pose[8].t.y() - .3f) < 1e-5f);
        auto lowerKeys = d.scene.layers[0].keys;
        create("Upper");
        EditService::execute(
            d, "motion_correction",
            {{"operation", "set"}, {"first", 10}, {"last", 50}, {"fadeIn", 5}, {"fadeOut", 5}});
        lift(30, .3f);
        auto upperKeys = d.scene.layers[1].keys;
        EditService::execute(d, "motion_correction", {{"operation", "select"}, {"layer", 0}});
        lift(30, .1f);
        QVERIFY(std::abs(d.scene.pose[8].t.y() - source.pose[8].t.y() - .8f) < 1e-5f);
        QCOMPARE(d.scene.layers[1].keys[30].offsets[8].t, upperKeys[30].offsets[8].t);
        QCOMPARE(sceneToJson(d.scene)["keys"], sourceJson);
        EditService::execute(d, "motion_correction",
                             {{"operation", "key_copy"}, {"from", 30}, {"frame", 20}});
        QVERIFY(d.scene.layers[0].keys.contains(20));
        d.undo.undo();
        QVERIFY(!d.scene.layers[0].keys.contains(20));
        d.undo.redo();
        EditService::execute(d, "motion_correction",
                             {{"operation", "key_move"}, {"from", 20}, {"frame", 25}});
        QVERIFY(!d.scene.layers[0].keys.contains(20));
        QVERIFY(d.scene.layers[0].keys.contains(25));
        EditService::execute(d, "motion_correction", {{"operation", "set"}, {"enabled", false}});
        d.scene.evaluate(30);
        QVERIFY(std::abs(d.scene.pose[8].t.y() - source.pose[8].t.y() - .3f) < 1e-5f);
        d.undo.undo();
        for (float f : {0.f, 12.5f, 25.f, 40.5f, 60.f}) {
            d.scene.evaluate(f);
            auto expected = d.scene.pose;
            d.scene.evaluate(59);
            d.scene.evaluate(2);
            d.scene.evaluate(f);
            for (size_t n = 0; n < expected.size(); ++n) {
                QVERIFY((expected[n].t - d.scene.pose[n].t).length() < 1e-6f);
                QVERIFY(std::abs(QQuaternion::dotProduct(expected[n].r, d.scene.pose[n].r)) > .99999f);
            }
        }
        EditService::execute(d, "motion_correction", {{"operation", "compare"}, {"enabled", true}});
        d.scene.evaluate(30);
        QCOMPARE(d.scene.pose[8].t, source.pose[8].t);
    }
    void rotationsMorphsAndUpperWorldIsolation() {
        Document d;
        auto source = Scene::mannequin();
        source.nodes[8].morphRest = {0};
        source.morphs[8] = {0};
        source.last = 60;
        source.keys[0] = {source.pose, {}, source.morphs};
        source.keys[60] = source.keys[0];
        d.replace(source);
        EditService::execute(d, "motion_correction",
                             {{"operation", "create"}, {"fadeIn", 0}, {"fadeOut", 0}});
        d.seek(0);
        EditService::execute(d, "pose_transform", {{"node", 6}, {"rotation", QJsonArray{0, 0, 170}}});
        d.seek(30);
        EditService::execute(d, "pose_transform", {{"node", 6}, {"rotation", QJsonArray{0, 0, -170}}});
        d.scene.evaluate(15);
        QVERIFY(std::abs(d.scene.pose[6].r.scalar()) < .001f);
        d.seek(30);
        EditService::execute(d, "morph_set", {{"node", 8}, {"target", 0}, {"weight", .4}});
        EditService::execute(d, "motion_correction",
                             {{"operation", "create"}, {"space", "world"}, {"fadeIn", 0}, {"fadeOut", 0}});
        EditService::execute(d, "pose_transform", {{"node", 6}, {"rotation", QJsonArray{20, 35, 45}}});
        auto upper = d.scene.layers[1].keys;
        auto desired = QQuaternion::fromEulerAngles(13, 22, 37);
        EditService::execute(d, "motion_correction", {{"operation", "select"}, {"layer", 0}});
        EditService::execute(d, "pose_transform", {{"node", 6}, {"rotation", QJsonArray{13, 22, 37}}});
        QVERIFY(std::abs(QQuaternion::dotProduct(d.scene.pose[6].r, desired)) > .99999f);
        QCOMPARE(d.scene.layers[1].keys[30].offsets[6].r, upper[30].offsets[6].r);
        QVERIFY(std::abs(d.scene.morphWeights(8)[0] - .4f) < 1e-5f);
        auto actual = d.scene;
        QString error;
        QTemporaryDir tmp;
        QVERIFY2(saveProject(tmp.path() + "/layers.jst", actual, error), qPrintable(error));
        Scene loaded;
        QVERIFY2(loadProject(tmp.path() + "/layers.jst", loaded, error), qPrintable(error));
        QCOMPARE(loaded.layers.size(), size_t(2));
        for (float f : {0.f, 15.f, 29.5f, 30.f, 60.f}) {
            actual.evaluate(f);
            loaded.evaluate(f);
            for (int n : {6, 8}) {
                QVERIFY((actual.pose[n].t - loaded.pose[n].t).length() < 1e-5f);
                QVERIFY(std::abs(QQuaternion::dotProduct(actual.pose[n].r, loaded.pose[n].r)) > .99999f);
            }
        }
    }
    void orderingAndSparseKeyDeletion() {
        Document d;
        auto source = Scene::mannequin();
        source.nodes[0].morphRest = {0};
        source.morphs[0] = {0};
        source.last = 60;
        source.keys[0] = {source.pose, {}, source.morphs};
        source.keys[60] = source.keys[0];
        d.replace(source);
        for (auto rotation : {QVector3D(40, 0, 0), QVector3D(0, 50, 0)}) {
            EditService::execute(d, "motion_correction",
                                 {{"operation", "create"}, {"fadeIn", 0}, {"fadeOut", 0}});
            auto &c = d.scene.correction();
            Transform offset;
            offset.s = {0, 0, 0};
            offset.r = QQuaternion::fromEulerAngles(rotation);
            c.keys[0].offsets[0] = offset;
            c.keys[60].offsets[0] = offset;
            c.keys[0].morphOffsets[0] = {0};
            c.keys[60].morphOffsets[0] = {.4f};
        }
        d.scene.evaluate(30);
        auto before = d.scene.pose[0].r;
        QVERIFY(std::abs(d.scene.morphWeights(0)[0] - .4f) < 1e-5f);
        EditService::execute(d, "motion_correction", {{"operation", "reorder"}, {"index", 0}});
        auto reordered = d.scene.pose[0].r;
        QVERIFY(std::abs(QQuaternion::dotProduct(before, reordered)) < .995f);
        d.undo.undo();
        QVERIFY(std::abs(QQuaternion::dotProduct(d.scene.pose[0].r, before)) > .99999f);
        EditService::execute(d, "motion_correction", {{"operation", "duplicate"}});
        QCOMPARE(d.scene.layers.size(), size_t(3));
        QVERIFY(d.scene.layers[1].id != d.scene.layers[2].id);
        d.undo.undo();
        EditService::execute(d, "motion_correction", {{"operation", "key_delete"}, {"from", 0}});
        EditService::execute(d, "motion_correction", {{"operation", "key_delete"}, {"from", 60}});
        d.scene.evaluate(30);
        QVERIFY(std::abs(d.scene.morphWeights(0)[0] - .2f) < 1e-5f);
        QCOMPARE(sceneToJson(d.scene)["keys"].toArray(), sceneToJson(source)["keys"].toArray());
    }
    void legacyLayerMigrationAndResampling() {
        auto source = Scene::mannequin();
        source.last = 60;
        source.keys[0] = {source.pose, {}, {}};
        source.keys[60] = source.keys[0];
        QString error;
        QVERIFY(source.beginCorrection(10, 50, 10, 10, false, error));
        Transform offset;
        offset.t = {0, .25f, 0};
        offset.s = {0, 0, 0};
        source.correction().offsets[8] = offset;
        auto json = sceneToJson(source);
        json["version"] = 3;
        json["correction"] = json["layers"].toArray()[0];
        json.remove("layers");
        json.remove("activeLayer");
        Scene migrated;
        QVERIFY2(sceneFromJson(json, migrated, error), qPrintable(error));
        QCOMPARE(migrated.layers.size(), size_t(1));
        for (float f : {0.f, 10.f, 15.f, 30.f, 45.f, 50.f, 60.f}) {
            source.evaluate(f);
            migrated.evaluate(f);
            QCOMPARE(source.pose[8].t, migrated.pose[8].t);
        }
        auto resampled = migrated.resampledCopy(48);
        QCOMPARE(resampled.fps, 48);
        QVERIFY(resampled.layers.empty());
        for (int f = 0; f <= 96; f += 7) {
            resampled.evaluate(float(f));
            migrated.evaluate(float(f) * 30 / 48);
            QVERIFY((resampled.pose[8].t - migrated.pose[8].t).length() < 1e-5f);
        }
    }
    void portableDemonstrations() {
        QString error;
        QTemporaryDir examples;
        QVERIFY(examples.isValid());
        for (QString kind : {"walk", "jump", "hand"}) {
            auto scene = mvpDemo(kind);
            QVERIFY2(scene.validate(&error), qPrintable(kind + ": " + error));
            QString base = examples.path()+"/"+kind+"-correction";
            QVERIFY(saveProject(base + ".jst", scene, error));
            Scene opened;
            QVERIFY(loadProject(base + ".jst", opened, error));
            QCOMPARE(opened.layers.size(), scene.layers.size());
            QCOMPARE(opened.contacts.size(), scene.contacts.size());
            auto output = opened.resampledCopy(48);
            QString folder = "artifacts/mvp-demos/" + kind;
            QDir().mkpath(folder);
            QVERIFY2(exportAnimation(folder + "/result.glb", output, error), qPrintable(error));
            QVERIFY2(exportAnimation(folder + "/result.fbx", output, error), qPrintable(error));
            QJsonArray reference;
            for (float frame : {0.f, 7.5f, 22.5f, 37.5f, 60.f}) {
                scene.evaluate(frame);
                opened.evaluate(frame);
                output.evaluate(frame * 48 / 30);
                auto a = scene.positions(scene.pose), b = opened.positions(opened.pose);
                QJsonObject joints, transforms;
                auto globals = output.globals(output.pose);
                for (size_t n = 0; n < a.size(); ++n) {
                    QVERIFY((a[n] - b[n]).length() < 1e-5f);
                    auto p = output.positions(output.pose)[n];
                    joints[scene.nodes[n].name] = QJsonArray{p.x(), p.y(), p.z()};
                    QJsonArray matrix;
                    for (int i = 0; i < 16; ++i)
                        matrix.append(globals[n].constData()[i]);
                    transforms[scene.nodes[n].name] = matrix;
                }
                QJsonArray vertices;
                for (const auto &mesh : output.meshes)
                    for (auto v : output.skinned(mesh))
                        vertices.append(QJsonArray{v.x(), v.y(), v.z()});
                reference.append(QJsonObject{{"seconds", double(frame) / 30},
                                             {"joints", joints},
                                             {"transforms", transforms},
                                             {"vertices", vertices}});
            }
            QFile expected(folder + "/expected.json");
            QVERIFY(expected.open(QIODevice::WriteOnly));
            expected.write(QJsonDocument(reference).toJson(QJsonDocument::Compact));
            if (kind == "walk") {
                QVERIFY(scene.layers.size() >= 2);
                QVERIFY(scene.contacts.size() >= 2);
                auto report = scene.diagnoseContacts();
                qInfo() << "Walk contacts:" << report.position << report.angle;
            }
            if (kind == "jump") {
                for (const auto &contact : scene.contacts)
                    QVERIFY(contactWeight(contact, 30) == 0);
            }
            if (kind == "hand")
                QVERIFY(scene.correction().keys.size() >= 5);
        }
    }
    void sharedServiceRejectsInvalidIndicesAtomically() {
        Document d;
        auto scene = Scene::mannequin();
        scene.nodes[0].morphRest = {0};
        scene.morphs[0] = {0};
        d.replace(scene);
        auto revision = d.revision;
        QVERIFY_EXCEPTION_THROWN(EditService::execute(d, "contact", {{"operation", "remove"}, {"index", -1}}),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(
            EditService::execute(d, "morph_set", {{"node", 0}, {"target", -1}, {"weight", 1}}),
            std::runtime_error);
        QCOMPARE(d.revision, revision);
        QVERIFY(!d.dirty());
        QCOMPARE(d.scene.morphWeights(0)[0], 0.f);
    }
    void originalComparisonBypassesAddedConstraints() {
        auto scene = mvpDemo("walk");
        scene.evaluate(12);
        auto corrected = scene.positions(scene.pose)[15];
        scene.compareOriginal = true;
        scene.evaluate(12);
        auto source = scene.motionOnlyCopy();
        source.compareOriginal = false;
        source.clearCorrections();
        source.contacts.clear();
        source.pins.clear();
        source.evaluate(12);
        QVERIFY((scene.positions(scene.pose)[15] - source.positions(source.pose)[15]).length() < 1e-5f);
        scene.evaluate(18);
        source.evaluate(18);
        QVERIFY((scene.positions(scene.pose)[15] - source.positions(source.pose)[15]).length() < 1e-5f);
        auto baked = scene.resampledCopy(48);
        baked.evaluate(12 * 48.f / 30);
        float error = (baked.positions(baked.pose)[15] - corrected).length();
        qInfo() << "Original view / 48 FPS bake fractional contact error (m):" << error;
        // Frame 12 of the source is frame 19.2 of the bake: FK interpolation between
        // discrete samples is approximate, with the same 1 mm acceptance as exports.
        QVERIFY2(error < .001f, qPrintable(QString::number(error)));
        auto report = scene.diagnoseContacts();
        QVERIFY(report.position < .001f);
    }
    void importedKeyInsertionMatchesDocumentShortcut() {
        Document shortcut, service;
        auto source = mvpDemo("hand");
        source.clearCorrections();
        source.evaluate(0);
        shortcut.replace(source);
        service.replace(source);
        shortcut.select(8);
        service.select(8);
        shortcut.insertKey();
        EditService::execute(service, "timeline", {{"operation", "insert"}});
        for (auto d : {&shortcut, &service}) {
            QCOMPARE(d->undo.count(), 1);
            QVERIFY(d->scene.correction().keys.contains(0));
            for (int n : {6, 7, 8})
                QVERIFY(d->scene.correction().keys[0].offsets.contains(n));
        }
        shortcut.seek(30);
        auto v = shortcut.scene.positions(shortcut.scene.pose)[8] + QVector3D(-.1, .1, .1);
        EditService::execute(shortcut, "pose_ik", {{"node", 8}, {"target", QJsonArray{v.x(), v.y(), v.z()}}});
        shortcut.scene.evaluate(0);
        source.evaluate(0);
        for (size_t n = 0; n < source.pose.size(); ++n)
            QVERIFY(std::abs(QQuaternion::dotProduct(shortcut.scene.pose[n].r, source.pose[n].r)) > .99999f);
    }
    void realCrashRecoveryKeepsOriginal() {
        QTemporaryDir dir;
        QString error, path = dir.path() + "/original.jst", folder = dir.path() + "/recovery";
        QVERIFY(saveProject(path, Scene::mannequin(), error));
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        auto bytes = original.readAll();
        original.close();
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--crash-autosave", folder, path});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 73);
        Workspace workspace;
        RecoveryStore store(workspace, folder);
        QCOMPARE(store.snapshots().size(), 1);
        auto snapshot = store.snapshots()[0];
        QVERIFY2(store.recover(snapshot, error), qPrintable(error));
        auto &d = workspace.active();
        QVERIFY(d.dirty());
        QVERIFY(d.path.isEmpty());
        QCOMPARE(d.frame, 24);
        QVERIFY(std::abs(d.scene.pose[0].t.x() - .37f) < 1e-5f);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), bytes);
        original.close();
        d.begin();
        d.scene.pose[0].t.setX(99);
        QVERIFY(store.flush(error));
        d.cancel();
        Scene invalid = d.scene;
        invalid.fps = 0;
        QVERIFY(!saveProject(path, invalid, error));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), bytes);
        original.close();
        QVERIFY(saveProject(dir.path() + "/recovered.jst", d.scene, error));
        d.undo.setClean();
        QVERIFY(store.flush(error));
        QVERIFY(store.snapshots().empty());
    }
    void fullHumanoidPassiveFollowAndFlight() {
        Scene s = Scene::mannequin();
        s.last = 30;
        s.profile = s.suggestedProfile();
        s.profile.configured = true;
        s.bodies = fittedPhysicsRig(s);
        QVERIFY(s.bodies.size() >= 12);
        s.physics.ground = -100;
        s.physics.gravity = 0;
        s.keys[0] = {s.pose, {}, {}};
        s.keys[10] = s.keys[0];
        s.pose[6].r = QQuaternion::fromAxisAndAngle(0, 0, 1, 45);
        s.pose[10].r = QQuaternion::fromAxisAndAngle(0, 0, 1, -35);
        s.keys[20] = {s.pose, {}, {}};
        s.keys[30] = s.keys[20];
        s.evaluate(0);
        Scene passive, follow;
        PhysicsReport a, b;
        QString error;
        QVERIFY2(simulatePhysics(s, 0, 30, passive, a, error), qPrintable(error));
        s.physics.followAnimation = true;
        QVERIFY2(simulatePhysics(s, 0, 30, follow, b, error), qPrintable(error));
        QCOMPARE(a.bodies, int(s.bodies.size()));
        QVERIFY(b.following);
        passive.evaluate(30);
        follow.evaluate(30);
        float angle = std::abs(QQuaternion::dotProduct(passive.pose[6].r, follow.pose[6].r));
        QVERIFY(angle < .999f);
        auto initial = physicalCenterOfMass(s), final = physicalCenterOfMass(follow);
        QVERIFY2((initial - final).length() < .015f, qPrintable(QString::number((initial - final).length())));
        s.physics.gravity = 9.81f;
        Scene flight;
        PhysicsReport report;
        QVERIFY(simulatePhysics(s, 0, 15, flight, report, error));
        flight.evaluate(15);
        auto fallen = physicalCenterOfMass(flight);
        QVERIFY(fallen.y() < initial.y() - .8f);
        Scene release = s;
        release.evaluate(0);
        auto target = release.positions(release.pose)[15];
        release.contacts = {{15, 0, 5, target, release.defaultPole(15)}};
        Scene held = release;
        held.contacts[0].last = 30;
        Scene releasedResult, heldResult;
        PhysicsReport releaseReport, heldReport;
        QVERIFY2(simulatePhysics(release, 0, 30, releasedResult, releaseReport, error), qPrintable(error));
        QVERIFY2(simulatePhysics(held, 0, 30, heldResult, heldReport, error), qPrintable(error));
        releasedResult.evaluate(30);
        heldResult.evaluate(30);
        QVERIFY(physicalCenterOfMass(releasedResult).y() < physicalCenterOfMass(heldResult).y() - .2f);
        qInfo() << "Full-body released / held COM:" << physicalCenterOfMass(releasedResult).y()
                << physicalCenterOfMass(heldResult).y();
        PhysicsControl cancel;
        cancel.cancel = true;
        Scene untouched = Scene::mannequin();
        auto name = untouched.name;
        QVERIFY(!simulatePhysics(s, 0, 30, untouched, report, error, &cancel));
        QCOMPARE(untouched.name, name);
        qInfo() << "Full-body Follow:" << b.summary();
    }
    void jobsKeepSourceIdentity() {
        Workspace workspace;
        workspace.append(Scene::mannequin());
        auto &a = workspace.active();
        a.scene.bodies = fittedPhysicsRig(a.scene);
        a.scene.physics.ground = -20;
        PhysicsJobs jobs(workspace);
        QString error;
        QVERIFY2(jobs.start(a, false, 0, 12, error), qPrintable(error));
        QString id = a.id;
        auto revision = a.revision;
        workspace.append(Scene::mannequin(), {}, false);
        QTRY_VERIFY_WITH_TIMEOUT((jobs.poll(), jobs.state()["done"].toBool()), 10000);
        QCOMPARE(jobs.state()["documentId"].toString(), id);
        QCOMPARE(jobs.state()["status"].toString(), QString("ready"));
        QVERIFY(!jobs.result(workspace.active()));
        workspace.activate(0);
        QVERIFY(jobs.result(a));
        QCOMPARE(a.revision, revision);
        EditService::execute(a, "pose_transform", {{"node", 0}, {"position", QJsonArray{0, 2, 0}}});
        QVERIFY(!jobs.result(a));
        QCOMPARE(jobs.state()["status"].toString(), QString("stale"));
        QVERIFY(jobs.start(a, false, 0, 90, error));
        jobs.cancel();
        QTRY_VERIFY_WITH_TIMEOUT((jobs.poll(), jobs.state()["done"].toBool()), 10000);
        QCOMPARE(jobs.state()["status"].toString(), QString("cancelled"));
        QVERIFY(jobs.start(a, false, 0, 90, error));
        workspace.remove(0);
        QTRY_VERIFY_WITH_TIMEOUT((jobs.poll(), jobs.state()["done"].toBool()), 10000);
        QCOMPARE(jobs.state()["status"].toString(), QString("stale"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--crash-autosave") {
        Workspace workspace;
        Scene scene;
        QString error;
        if (!loadProject(args[3], scene, error))
            return 91;
        workspace.append(std::move(scene), args[3]);
        auto &d = workspace.active();
        d.seek(24);
        EditService::execute(d, "pose_transform", {{"node", 0}, {"position", QJsonArray{.37, 1.04, 0}}});
        RecoveryStore store(workspace, args[2]);
        if (!store.flush(error))
            return 92;
        std::_Exit(73); // No destructors / normal application shutdown: emulate process loss.
    }
    MVPTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "mvp_tests.moc"
