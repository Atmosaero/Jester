#include "appearance.h"
#include "window.h"
#include "notes_ui.h"
#include "notes_card.h"
#include "motion_annotations.h"
#include "edit_service.h"
#include "editor_api.h"
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QMenu>
#include <QDir>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QToolButton>
#include <QtTest>
using namespace trickster;
class NotesTests:public QObject {
    Q_OBJECT
    void expose(Window &w){
        w.document().replace(Scene::mannequin());
        w.resize(qRound(1280/w.devicePixelRatioF()),qRound(720/w.devicePixelRatioF()));
        w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));w.activateWindow();w.viewport->fit();QTest::qWait(70);
    }
    void finish(Window &w){for(auto &d:w.workspace.documents)d->undo.setClean();w.close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
    NotesController *notes(Window &w){return w.findChild<NotesController*>();}
    NotesCard *card(Window &w){for(auto c:w.findChildren<NotesCard*>())if(c->isVisible())return c;return nullptr;}
    void dismiss(Window &w){if(auto c=card(w))QTest::keyClick(c->findChild<QTextEdit*>("noteText"),Qt::Key_Escape);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
    QString add(Window &w,QString text="Toe slightly higher; heel stays on the floor"){
        notes(w)->addNote();auto c=card(w);if(!c)return {};
        c->findChild<QTextEdit*>("noteText")->setPlainText(text);
        QTest::keyClick(c->findChild<QTextEdit*>("noteText"),Qt::Key_Return,Qt::ControlModifier);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        return w.document().scene.annotations.empty()?QString():w.document().scene.annotations.last().toObject()["id"].toString();
    }
    QJsonObject tool(EditorApi &api,const QString &name,const QJsonObject &args){
        auto result=api.call(name,args);
        if(result["isError"].toBool())QTest::qFail(qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())),__FILE__,__LINE__);
        return result["structuredContent"].toObject();
    }
    QJsonObject intent(double start,double end){
        QJsonArray keys{
            QJsonObject{{"seconds",start},{"target",QJsonArray{0,0,0}}},
            QJsonObject{{"seconds",end},{"target",QJsonArray{1,0,0}}}
        };
        QJsonObject expectation{{"kind","position"},{"units","metres"},{"space","world"},
            {"tolerance",.01},{"futureField","preserved"},{"targetKeys",keys}};
        return {{"name","Legacy foot note"},{"description","Keep heel low"},{"start",start},{"end",end},
            {"nodes",QJsonArray{15}},{"provenance","hypothesis"},{"type","pose"},
            {"vendorExtension",QJsonObject{{"keep",42}}},{"tags",QJsonArray{"review","foot"}},
            {"expectations",QJsonArray{expectation}}};
    }
  private slots:
    void initTestCase(){QSettings().clear();QDir().mkpath("artifacts/notes");}
    void quickFootNoKeyAndKeyboard(){
        Window w;expose(w);auto &d=w.document();d.seek(22);d.select(15);
        auto keys=d.scene.keys;auto pose=d.scene.pose;auto motion=d.motionRevision;auto mesh=d.meshRevision;
        QVERIFY(!keys.contains(22));
        w.viewport->setFocus();QTest::keyClick(w.viewport,Qt::Key_N,Qt::ControlModifier|Qt::AltModifier);QTRY_VERIFY(card(w));
        auto c=card(w);auto text=c->findChild<QTextEdit*>("noteText");QTRY_VERIFY(text->hasFocus());
        QVERIFY(c->findChild<QLabel*>("noteAddress")->text().contains("LeftFoot"));
        QTest::keyClicks(text,"W E I P Q F 123");QTest::keyClick(text,Qt::Key_Return);QTest::keyClicks(text,"Second line");
        QCOMPARE(d.undo.count(),0);QVERIFY(!d.playing);QCOMPARE(d.frame,22);QCOMPARE(w.viewport->tool,Viewport::Move);
        QTest::keyClick(text,Qt::Key_Return,Qt::ControlModifier);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QCOMPARE(d.undo.count(),1);QCOMPARE(d.scene.annotations.size(),1);auto v=d.scene.annotations[0].toObject();
        QCOMPARE(v["name"].toString(),QString("W E I P Q F 123"));QCOMPARE(v["description"].toString(),QString("W E I P Q F 123\nSecond line"));
        QCOMPARE(v["nodes"].toArray(),QJsonArray{15});QCOMPARE(v["start"].toDouble(),22./30);QCOMPARE(v["start"],v["end"]);QVERIFY(!v.contains("attachment"));
        QCOMPARE(d.motionRevision,motion);QCOMPARE(d.meshRevision,mesh);QCOMPARE(d.scene.keys.size(),keys.size());
        for(size_t n=0;n<pose.size();++n){QCOMPARE(d.scene.pose[n].t,pose[n].t);QCOMPARE(d.scene.pose[n].r,pose[n].r);}
        finish(w);
    }
    void viewportContextMenu(){
        Window w;expose(w);auto &d=w.document();d.select(-1);d.seek(22);
        auto anchor=w.viewport->project(d.scene.positions(d.scene.pose)[15]).toPoint();
        QTest::mouseClick(w.viewport,Qt::RightButton,Qt::NoModifier,anchor);
        bool clicked=false;
        QTimer::singleShot(50,&w,[&]{auto menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;
            for(auto action:menu->actions())if(action->text()=="Add note"){clicked=true;QTest::mouseClick(menu,Qt::LeftButton,Qt::NoModifier,menu->actionGeometry(action).center());return;}
            menu->close();});
        QContextMenuEvent event(QContextMenuEvent::Mouse,anchor,w.viewport->mapToGlobal(anchor));QApplication::sendEvent(w.viewport,&event);
        QVERIFY(clicked);QVERIFY(card(w));QCOMPARE(card(w)->original["nodes"].toArray(),QJsonArray{15});QCOMPARE(d.frame,22);dismiss(w);finish(w);
    }
    void multiBoneCharacterAndInterval(){
        Window w;expose(w);auto &d=w.document();d.setSelection({15,19},19);d.seek(12);
        d.selectTime(.2,.8);add(w,"Both feet");auto v=d.scene.annotations.last().toObject();QCOMPARE(v["nodes"].toArray(),(QJsonArray{19,15}));QCOMPARE(v["start"].toDouble(),.2);QCOMPARE(v["end"].toDouble(),.8);
        d.timeSelection.reset();d.select(-1);add(w,"Whole character");v=d.scene.annotations.last().toObject();QVERIFY(v["nodes"].toArray().empty());
        d.select(8);d.selectAnnotation(v["id"].toString());QCOMPARE(d.selected,-1);QVERIFY(d.selection().empty());dismiss(w);
        finish(w);
    }
    void frozenDraftAndDocuments(){
        Window w;expose(w);auto *source=&w.document();source->seek(22);source->select(15);notes(w)->addNote();auto draft=card(w);QVERIFY(draft);
        draft->findChild<QTextEdit*>("noteText")->setPlainText("Captured foot and frame");source->seek(31);source->select(8);
        QCOMPARE(draft->original["nodes"].toArray(),QJsonArray{15});QCOMPARE(draft->original["start"].toDouble(),22./30);
        w.workspace.append(Scene::mannequin(),{},false);QVERIFY(!draft->isVisible());QVERIFY(w.document().scene.annotations.empty());
        w.workspace.activate(0);QTRY_VERIFY(draft->isVisible());QCOMPARE(draft->findChild<QTextEdit*>("noteText")->toPlainText(),QString("Captured foot and frame"));
        QTest::keyClick(draft->findChild<QTextEdit*>("noteText"),Qt::Key_Return,Qt::ControlModifier);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QCOMPARE(source->scene.annotations.size(),1);QVERIFY(w.workspace.documents[1]->scene.annotations.empty());QCOMPARE(source->scene.annotations[0].toObject()["start"].toDouble(),22./30);QCOMPARE(source->scene.annotations[0].toObject()["nodes"].toArray(),QJsonArray{15});
        source->select(15);notes(w)->addNote();QPointer<NotesCard> doomed=card(w);QVERIFY(doomed);w.workspace.remove(0);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(!doomed);QVERIFY(w.document().scene.annotations.empty());
        finish(w);
    }
    void preservationUndoAtOtherFrameAndReopen(){
        Window w;expose(w);auto &d=w.document();d.scene.keys[0]={d.scene.pose,{},d.scene.morphs};auto original=intent(.123456789012345,.923456789012345);
        auto result=EditService::execute(d,"motion_annotations",{{"operation","add"},{"annotation",original}});auto id=result["annotation"].toObject()["id"].toString();
        EditService::execute(d,"motion_annotations",{{"operation","attach"},{"id",id},{"frame",0}});original=d.scene.annotations[0].toObject();
        d.selectAnnotation(id);auto c=card(w);QVERIFY(c);c->findChild<QTextEdit*>("noteText")->setPlainText("Heel stays planted\n"+QString(1000,'x'));auto undo=d.undo.count();auto motion=d.motionRevision;
        QTest::keyClick(c->findChild<QTextEdit*>("noteText"),Qt::Key_Return,Qt::ControlModifier);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QCOMPARE(d.undo.count(),undo+1);auto edited=d.scene.annotations[0].toObject();
        for(auto field:{"vendorExtension","expectations","attachment","provenance","tags","start","end","nodes"})QCOMPARE(edited[field],original[field]);
        QCOMPARE(edited["description"].toString().size(),1019); // Includes the complete second line.
        d.seek(47);auto pose=d.scene.pose;d.undo.undo();QCOMPARE(d.frame,47);QCOMPARE(d.scene.annotations[0].toObject(),original);QCOMPARE(d.motionRevision,motion);QCOMPARE(d.scene.pose[0].t,pose[0].t);
        d.undo.redo();QCOMPARE(d.frame,47);QCOMPARE(d.scene.annotations[0].toObject(),edited);
        QTemporaryDir tmp;QString error;QVERIFY2(saveProject(tmp.path()+"/notes.trickster",d.scene,error),qPrintable(error));Scene loaded;QVERIFY2(loadProject(tmp.path()+"/notes.trickster",loaded,error),qPrintable(error));QCOMPARE(loaded.annotations,d.scene.annotations);
        finish(w);
    }
    void frameBinsFpsAndTargetRetime(){
        Document d;d.replace(Scene::mannequin());auto result=EditService::execute(d,"motion_annotations",{{"operation","add"},{"annotation",intent(.51,.91)}});auto id=result["annotation"].toObject()["id"].toString();
        EditService::execute(d,"motion_annotations",{{"operation","set"},{"id",id},{"annotation",QJsonObject{{"start",1.1},{"end",1.9}}}});
        auto targets=d.scene.annotations[0].toObject()["expectations"].toArray()[0].toObject()["targetKeys"].toArray();QVERIFY(std::abs(targets[0].toObject()["seconds"].toDouble()-1.1)<1e-12);QVERIFY(std::abs(targets[1].toObject()["seconds"].toDouble()-1.9)<1e-12);
        EditService::execute(d,"timeline",{{"operation","range"},{"first",0},{"last",120},{"fps",60}});QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),1.1);
        QJsonObject point{{"start",.51},{"end",.51}};QVERIFY(annotationActive(point,31./60,60));QVERIFY(!annotationActive(point,30./60,60));QCOMPARE(annotationFrame(-.5/60,60),0);
        auto p=editAnnotations(d.scene,{{"operation","add"},{"annotation",QJsonObject{{"name","Point between frames"},{"start",.51},{"end",.51},{"nodes",QJsonArray{15}}}}})["annotation"].toObject();
        d.selectAnnotation(p["id"].toString());QCOMPARE(d.frame,31);QCOMPARE(motionContext(d.scene,31./60)["active"].toArray().size(),1);
        QCOMPARE(d.scene.annotations[0].toObject()["start"].toDouble(),1.1);
    }
    void compactMcpAndMissingBindings(){
        Window w;expose(w);auto &d=w.document();auto value=intent(.51,.51);value["expectations"]=QJsonArray();value["description"]=QString(700,'z');value["nodes"]=QJsonArray{15,999};
        auto result=EditService::execute(d,"motion_annotations",{{"operation","add"},{"annotation",value}});auto id=result["annotation"].toObject()["id"].toString();
        EditorApi api(&w);auto compact=tool(api,"motion_context",{{"seconds",15./30}});auto note=compact["active"].toArray()[0].toObject();
        QCOMPARE(note["id"].toString(),id);QCOMPARE(note["description"].toString().size(),256);QVERIFY(note["textTruncated"].toBool());QCOMPARE(note["textLength"].toInt(),700);QCOMPARE(note["nodeNames"].toArray()[0].toString(),QString("LeftFoot"));QVERIFY(note["missingBindings"].toBool());QVERIFY(note["point"].toBool());QCOMPARE(note["startFrame"].toInt(),15);QCOMPARE(note["start"].toDouble(),.51);
        auto full=tool(api,"motion_context",{{"seconds",.5},{"detail","full"},{"id",id}})["active"].toArray()[0].toObject();QCOMPARE(full["description"].toString().size(),700);QVERIFY(!full["textTruncated"].toBool());
        auto all=tool(api,"motion_annotations",{{"operation","list"},{"id",id}})["annotations"].toArray();QCOMPARE(all[0].toObject()["description"].toString().size(),700);
        notes(w)->addNote();QVERIFY(card(w));
        tool(api,"ui_set",{{"id","noteText"},{"value","Written through the note card MCP control"}});
        auto inspected=tool(api,"ui_inspect",{{"id","noteText"}});QVERIFY(!inspected.isEmpty());
        auto input=card(w)->findChild<QTextEdit*>("noteText");QCOMPARE(input->toPlainText(),QString("Written through the note card MCP control"));
        QTest::keyClick(input,Qt::Key_Return,Qt::ControlModifier);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QCOMPARE(d.scene.annotations.size(),2);
        QString error;QTemporaryDir tmp;QVERIFY(saveProject(tmp.path()+"/orphan.trickster",d.scene,error));Scene reopened;QVERIFY(loadProject(tmp.path()+"/orphan.trickster",reopened,error));QCOMPARE(reopened.annotations,d.scene.annotations);
        finish(w);
    }
    void panelFiltersAndNoPlaybackRebuild(){
        Window w;expose(w);auto &d=w.document();d.select(15);d.seek(22);auto foot=add(w,"Heel review");d.select(8);d.seek(31);add(w,"Hand review");
        notes(w)->showPanel();auto list=w.findChild<QListWidget*>("notesList");auto filter=w.findChild<QComboBox*>("notesFilter");auto search=w.findChild<QLineEdit*>("notesSearch");QCOMPARE(list->count(),2);
        auto first=list->item(0);d.seek(15);d.play(true);QTest::qWait(160);d.play(false);QCOMPARE(list->item(0),first);
        search->setText("Heel");QCOMPARE(list->count(),1);search->clear();filter->setCurrentIndex(1);QCOMPARE(list->count(),1);d.select(15);QCOMPARE(list->count(),1);QCOMPARE(list->item(0)->data(Qt::UserRole).toString(),foot);
        filter->setCurrentIndex(2);d.seek(22);QCOMPARE(list->count(),1);QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,list->visualItemRect(list->item(0)).center());QCOMPARE(d.activeAnnotation,foot);QCOMPARE(d.frame,22);QCOMPARE(d.selected,15);QVERIFY(card(w));dismiss(w);
        filter->setCurrentIndex(0);d.selectAnnotation(foot);dismiss(w);auto undo=d.undo.count();w.findChild<QPushButton*>("deleteNoteButton")->click();QCOMPARE(d.undo.count(),undo+1);QCOMPARE(d.scene.annotations.size(),1);d.undo.undo();QCOMPARE(d.scene.annotations.size(),2);
        finish(w);
    }
    void viewportMarkersCameraPoseTimelineAndCaptures(){
        Window w;expose(w);auto &d=w.document();d.seek(22);d.select(15);auto id=add(w);
        add(w,"The foot rolls smoothly");d.select(-1);add(w,"Overall timing review");d.select(15);
        notes(w)->showPanel();w.viewport->update();QTest::qWait(90);
        captureEditorWidget(&w).save("artifacts/notes/layout-debug.png");qInfo()<<"Viewport/dock/card bounds"<<w.viewport->size()<<w.findChild<QDockWidget*>("notesDock")->geometry();
        QTRY_VERIFY_WITH_TIMEOUT(!w.viewport->property("annotationMarkers").toMap().empty(),2000);
        auto markers=w.viewport->property("annotationMarkers").toMap();
        if(!markers.contains(id)){captureEditorWidget(&w).save("artifacts/notes/marker-missing.png");qInfo()<<"Marker state"<<w.viewport->size()<<d.frame<<d.scene.fps<<d.selected<<w.viewport->projectVisible(d.scene.positions(d.scene.pose)[15]).has_value()<<markers;}
        QVERIFY(markers.contains(id));QVERIFY(markers.size()>=3);
        auto rect=markers[id].toRect();QVERIFY(w.viewport->rect().contains(rect));
        for(auto it=markers.begin();it!=markers.end();++it)for(auto other=std::next(it);other!=markers.end();++other)QVERIFY(!it.value().toRect().intersects(other.value().toRect()));
        QTest::mouseClick(w.viewport,Qt::LeftButton,Qt::NoModifier,rect.center());QCOMPARE(d.activeAnnotation,id);QVERIFY(card(w));
        auto c=card(w);QVERIFY(w.viewport->rect().contains(c->geometry()));QVERIFY(c->findChild<QPushButton*>("noteSave")->isVisibleTo(c));
        auto dock=w.findChild<QDockWidget*>("notesDock");QVERIFY(dock->rect().contains(dock->widget()->geometry()));
        auto noteList=w.findChild<QListWidget*>("notesList");QVERIFY(noteList->viewport()->height()>=noteList->visualItemRect(noteList->item(0)).height());
        for(auto widget:QList<QWidget*>{w.findChild<QLineEdit*>("notesSearch"),w.findChild<QComboBox*>("notesFilter"),w.findChild<QListWidget*>("notesList"),w.findChild<QPushButton*>("addNoteButton")}){
            auto bounds=QRect(widget->mapTo(dock,QPoint()),widget->size());QVERIFY2(dock->rect().contains(bounds),qPrintable(widget->objectName()));
        }
        QSize originalSize=w.size();w.resize(std::max(760,w.width()-80),w.height());QTest::qWait(60);QVERIFY(w.viewport->rect().contains(c->geometry()));w.resize(originalSize);QTest::qWait(60);

        QString scale=QString::number(w.devicePixelRatioF());
        QDir().mkpath("docs/notes");captureEditorWidget(&w).save("docs/notes/card-"+scale+".png");dismiss(w);
        auto camera=w.viewport->camera();camera.yaw+=50;w.viewport->setCamera(camera);QTest::qWait(80);
        auto moved=w.viewport->property("annotationMarkers").toMap();QVERIFY(moved.contains(id));QVERIFY(moved[id].toRect()!=rect);
        auto before=moved[id].toRect();d.scene.pose[0].t+=QVector3D(.12f,.04f,0);emit d.changed();QTest::qWait(80);QVERIFY(w.viewport->property("annotationMarkers").toMap()[id].toRect()!=before);
        d.play(true);QTest::qWait(90);QVERIFY(w.viewport->property("annotationMarkers").toMap().empty());d.play(false);d.seek(22);QTest::qWait(60);
        auto action=w.findChild<QAction*>("noteMarkersAction");QVERIFY(action);action->setChecked(false);QTest::qWait(60);QVERIFY(w.viewport->property("annotationMarkers").toMap().empty());action->setChecked(true);
        d.selectAnnotation(id);dismiss(w);int y=w.timeline->contextTrackCenter(id);QVERIFY(y>22 && y<w.timeline->height()-22);
        QTest::mouseClick(w.timeline,Qt::LeftButton,Qt::NoModifier,QPoint(70,y));QVERIFY(card(w));QCOMPARE(d.activeAnnotation,id);dismiss(w);
        captureEditorWidget(&w).save("docs/notes/panel-"+scale+".png");
        // Behind the camera or outside the viewport: hide the bone marker, keep the panel row.
        d.scene.pose[0].t+=QVector3D(1000,0,0);emit d.changed();QTest::qWait(70);QVERIFY(!w.viewport->property("annotationMarkers").toMap().contains(id));
        QCOMPARE(w.findChild<QListWidget*>("notesList")->count(),3);
        finish(w);
    }
    void timelinePointMoveAndIntervalStretch(){
        Window w;expose(w);auto &d=w.document();d.select(15);d.seek(10);auto id=add(w,"Move this point");d.selectAnnotation(id);dismiss(w);w.timeline->fit();
        int y=w.timeline->contextTrackCenter(id);auto from=QPoint(qRound(w.timeline->framePosition(10)),y),to=QPoint(qRound(w.timeline->framePosition(15)),y);auto undo=d.undo.count();
        QTest::mousePress(w.timeline,Qt::LeftButton,Qt::NoModifier,from);QTest::mouseMove(w.timeline,to,30);QTest::mouseRelease(w.timeline,Qt::LeftButton,Qt::NoModifier,to);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QCOMPARE(d.undo.count(),undo+1);auto point=d.scene.annotations[0].toObject();QVERIFY(std::abs(point["start"].toDouble()-.5)<.01);QCOMPARE(point["start"],point["end"]);dismiss(w);
        auto result=EditService::execute(d,"motion_annotations",{{"operation","add"},{"annotation",intent(.4,1.)}});id=result["annotation"].toObject()["id"].toString();d.selectAnnotation(id);dismiss(w);w.timeline->fit();y=w.timeline->contextTrackCenter(id);
        from=QPoint(qRound(w.timeline->framePosition(30)),y);to=QPoint(qRound(w.timeline->framePosition(40)),y);undo=d.undo.count();
        QTest::mousePress(w.timeline,Qt::LeftButton,Qt::NoModifier,from);QTest::mouseMove(w.timeline,to,30);QTest::mouseRelease(w.timeline,Qt::LeftButton,Qt::NoModifier,to);
        QCOMPARE(d.undo.count(),undo+1);auto note=d.scene.annotations.last().toObject();auto keys=note["expectations"].toArray()[0].toObject()["targetKeys"].toArray();QVERIFY(std::abs(keys[1].toObject()["seconds"].toDouble()-note["end"].toDouble())<1e-12);QCOMPARE(keys[0].toObject()["seconds"].toDouble(),note["start"].toDouble());
        dismiss(w);finish(w);
    }
    void timelineDragCancelsOnDocumentSwitch(){
        Window w;expose(w);auto *source=&w.document();source->select(15);source->seek(10);auto id=add(w,"Stable document address");source->selectAnnotation(id);dismiss(w);w.timeline->fit();
        auto expected=source->scene.annotations;int y=w.timeline->contextTrackCenter(id);auto a=QPoint(qRound(w.timeline->framePosition(10)),y),b=QPoint(qRound(w.timeline->framePosition(15)),y);
        QTest::mousePress(w.timeline,Qt::LeftButton,Qt::NoModifier,a);QTest::mouseMove(w.timeline,b,30);
        w.workspace.append(source->scene,{},false);QTest::mouseRelease(w.timeline,Qt::LeftButton,Qt::NoModifier,b);
        QCOMPARE(w.document().scene.annotations,expected);QCOMPARE(source->scene.annotations,expected);QCOMPARE(w.document().undo.count(),0);finish(w);
    }
    void advancedBindingsAttachmentAndCardDelete(){
        Window w;expose(w);auto &d=w.document();d.scene.keys[0]={d.scene.pose,{},d.scene.morphs};d.select(15);d.seek(0);
        auto motion=d.motionRevision;auto count=d.undo.count();notes(w)->addNote();auto c=card(w);QVERIFY(c);
        c->findChild<QTextEdit*>("noteText")->setPlainText("Explicit key attachment");
        for(auto button:c->findChildren<QToolButton*>())if(button->text()=="Advanced")button->click();
        auto bones=c->findChild<QListWidget*>("noteBindings");QVERIFY(bones);
        for(int i=0;i<bones->count();++i){auto item=bones->item(i);item->setCheckState(item->data(Qt::UserRole).toInt()==8?Qt::Checked:Qt::Unchecked);}
        for(auto button:c->findChildren<QPushButton*>())if(button->text().startsWith("Attach to key"))button->click();
        QTest::keyClick(c->findChild<QTextEdit*>("noteText"),Qt::Key_Return,Qt::ControlModifier);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QCOMPARE(d.undo.count(),count+1);QCOMPARE(d.motionRevision,motion);QCOMPARE(d.scene.keys.size(),1);QCOMPARE(d.scene.annotations.size(),1);
        auto note=d.scene.annotations[0].toObject();QCOMPARE(note["nodes"].toArray(),QJsonArray{8});QCOMPARE(note["attachment"].toObject()["status"].toString(),QString("attached"));QVERIFY(!d.scene.keys[0].id.isEmpty());
        d.selectAnnotation(note["id"].toString());c=card(w);QVERIFY(c);QTest::mouseClick(c->findChild<QToolButton*>("noteDelete"),Qt::LeftButton);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QCOMPARE(d.undo.count(),count+2);QVERIFY(d.scene.annotations.empty());d.undo.undo();QCOMPARE(d.scene.annotations[0].toObject(),note);d.undo.undo();QVERIFY(d.scene.annotations.empty());QVERIFY(d.scene.keys[0].id.isEmpty());d.undo.redo();QCOMPARE(d.scene.annotations[0].toObject(),note);
        finish(w);
    }
    void staleEditRejectedAndCancel(){
        Window w;expose(w);w.document().select(15);auto id=add(w);w.document().selectAnnotation(id);auto c=card(w);QVERIFY(c);
        c->findChild<QTextEdit*>("noteText")->setPlainText("My pending review");
        EditService::execute(w.document(),"motion_annotations",{{"operation","set"},{"id",id},{"annotation",QJsonObject{{"description","Changed through MCP"}}}});
        QTest::keyClick(c->findChild<QTextEdit*>("noteText"),Qt::Key_Return,Qt::ControlModifier);QVERIFY(c->findChild<QLabel*>("noteError")->isVisible());QCOMPARE(w.document().scene.annotations[0].toObject()["description"].toString(),QString("Changed through MCP"));dismiss(w);
        auto count=w.document().undo.count();notes(w)->addNote();dismiss(w);QCOMPARE(w.document().undo.count(),count);finish(w);
    }
};
#include "atom_test_main.h"
int main(int argc,char** argv) { return runJesterTests<NotesTests>(argc,argv,"JesterNotesTests"); }
#include "notes_tests.moc"
