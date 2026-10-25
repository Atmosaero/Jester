#include "ui/notes/notes_ui.h"
#include "ui/notes/notes_card.h"
#include "ui/notes/notes_overlay.h"
#include "core/annotations/motion_annotations.h"
#include "core/scene/edit_service.h"
#include "ui/window/window.h"
#include "ui/widgets/icons.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
namespace trickster {
NotesController::NotesController(Window *w):QObject(w),window(w){
    setObjectName("notesController");
    dock=new QDockWidget("Notes",w);dock->setObjectName("notesDock");
    dock->setMinimumWidth(200);dock->setWindowIcon(icon(Glyph::Note));
    auto body=new QWidget;auto layout=new QVBoxLayout(body);layout->setContentsMargins(6,4,6,4);layout->setSpacing(4);
    auto toolbar=new QHBoxLayout;
    auto add=new QPushButton("+ Note");add->setObjectName("addNoteButton");add->setToolTip("Add note at selected bones and current frame (Ctrl+Alt+N)");
    auto remove=new QPushButton("Delete");remove->setObjectName("deleteNoteButton");
    toolbar->addWidget(add);toolbar->addStretch();toolbar->addWidget(remove);layout->addLayout(toolbar);
    search=new QLineEdit;search->setObjectName("notesSearch");search->setPlaceholderText("Search notes");layout->addWidget(search);
    filter=new QComboBox;filter->setObjectName("notesFilter");filter->addItems({"All notes","Selected bones","Current moment"});layout->addWidget(filter);
    list=new QListWidget;list->setObjectName("notesList");list->setWordWrap(true);list->setSpacing(3);list->setMinimumHeight(42);layout->addWidget(list,1);
    list->setToolTip("Select a note to visit its frame and edit. Ctrl+Enter saves the card; Esc cancels.");
    dock->setWidget(body);w->addDockWidget(Qt::RightDockWidgetArea,dock);
    if(auto outliner=w->findChild<QDockWidget*>("skeletonDock"))w->tabifyDockWidget(outliner,dock);
    dock->hide();
    auto addAction=new QAction("Add note",w);addAction->setObjectName("addNoteAction");addAction->setIcon(icon(Glyph::Note));addAction->setShortcut(QKeySequence("Ctrl+Alt+N"));w->addAction(addAction);
    connect(addAction,&QAction::triggered,this,&NotesController::addNote);
    connect(add,&QPushButton::clicked,this,&NotesController::addNote);
    connect(remove,&QPushButton::clicked,this,&NotesController::removeSelected);
    connect(search,&QLineEdit::textChanged,this,[this]{listSignature.clear();refresh();});
    connect(filter,&QComboBox::currentIndexChanged,this,[this]{listSignature.clear();refresh();});
    connect(list,&QListWidget::itemClicked,this,[this](QListWidgetItem *item){if(!rebuilding)window->document().selectAnnotation(item->data(Qt::UserRole).toString());});
    connect(list,&QListWidget::currentItemChanged,this,[this](QListWidgetItem *item){if(item && !rebuilding)window->document().selectAnnotation(item->data(Qt::UserRole).toString());});
    connect(list,&QListWidget::itemActivated,this,[this](QListWidgetItem *item){window->document().selectAnnotation(item->data(Qt::UserRole).toString());});
    for(auto action:w->menuBar()->actions())if(action->menu()){
        if(action->text().remove('&')=="Edit")action->menu()->addAction(addAction);
        if(action->text().remove('&')=="View"){
            action->menu()->addAction(dock->toggleViewAction());
            auto markers=action->menu()->addAction("Note markers");markers->setObjectName("noteMarkersAction");markers->setCheckable(true);markers->setChecked(true);
            connect(markers,&QAction::toggled,this,[this](bool on){window->viewport->showNotes=on;window->viewport->update();});
        }
    }
    overlay=new NotesOverlay(w->viewport,this);
    connect(w->viewport,&Viewport::addNoteRequested,this,&NotesController::addNote);
    connect(w->timeline,&Timeline::notesRequested,this,&NotesController::showPanel);
    connect(&w->workspace,&Workspace::activeChanged,this,&NotesController::bind);
    connect(&w->workspace,&Workspace::changed,this,&NotesController::refresh);
    connect(&w->workspace,&Workspace::selectionChanged,this,&NotesController::refresh);
    bind();
}
NotesController::~NotesController(){for(auto card:cards)if(card)delete card;}
void NotesController::bind(){
    if(bound)disconnect(bound,nullptr,this,nullptr);
    for(auto card:cards)if(card)card->hide();
    bound=&window->document();overlay->bind(bound);listSignature.clear();
    connect(bound,&Document::annotationSelected,this,[this](QString id){syncSelection();openNote(id);});
    connect(bound,&Document::sceneReplaced,this,[this]{
        auto card=cards.take(bound->id);if(card)card->deleteLater();listSignature.clear();refresh();
    });
    if(auto card=cards.value(bound->id)){placeCard(card);card->show();}
    refresh();
}
void NotesController::showPanel(){dock->show();dock->raise();refresh();}
void NotesController::addNote(){
    auto &d=window->document();
    if(window->viewport->isDragging() || d.inTransaction()){window->report("Finish the current pose edit before adding a note.");return;}
    d.play(false);
    if(auto card=cards.value(d.id)){
        if(card->dirty()){card->focusText();window->report("Save or cancel the open note first.");return;}
        delete card;
    }
    QJsonArray nodes;auto selected=d.selection().values();std::sort(selected.begin(),selected.end());
    if(d.selected>=0 && selected.contains(d.selected)){selected.removeAll(d.selected);selected.prepend(d.selected);}
    for(int n:selected)nodes.append(n);
    double a=double(d.frame)/d.scene.fps,b=a;
    if(d.timeSelection && d.timeSelection->second>d.timeSelection->first){a=d.timeSelection->first;b=d.timeSelection->second;}
    auto card=new NotesCard(window,this,&d,{{"nodes",nodes},{"start",a},{"end",b},{"type",a==b?"event":"phase"},{"provenance","intention"}},true);
    cards[d.id]=card;placeCard(card);card->focusText();
}
void NotesController::openNote(const QString &id){
    auto &d=window->document();
    if(auto card=cards.value(d.id)){
        if(card->original["id"]==id){card->focusText();return;}
        if(card->dirty()){card->focusText();window->report("Save or cancel the open note before editing another.");return;}
        delete card;
    }
    for(auto value:d.scene.annotations){auto v=value.toObject();if(v["id"]!=id)continue;
        auto card=new NotesCard(window,this,&d,v,false);cards[d.id]=card;placeCard(card);card->focusText();return;
    }
}
void NotesController::placeCard(NotesCard *card){
    if(!card || !card->document)return;
    auto viewport=window->viewport;
    int width=std::min({350,std::max(220,int(viewport->width()*.45)),viewport->width()-16});
    int height=std::min(card->property("notesExpanded").toBool()?420:240,std::max(64,viewport->height()-16));
    QPoint pos(viewport->width()-width-8,8);
    auto nodes=card->original["nodes"].toArray();
    if(!nodes.empty()){
        int node=nodes[0].toInt(-1);auto &s=card->document->scene;
        if(node>=0 && node<int(s.nodes.size()))if(auto anchor=viewport->projectVisible(s.positions(s.pose)[node])){
            pos=anchor->toPoint()+QPoint(32,-height/2);
            if(pos.x()+width>viewport->width()-8)pos.setX(int(anchor->x())-width-32);
        }
    }
    pos.setX(std::clamp(pos.x(),8,std::max(8,viewport->width()-width-8)));
    pos.setY(std::clamp(pos.y(),8,std::max(8,viewport->height()-height-8)));
    card->setGeometry(QRect(pos,QSize(width,height)));card->setProperty("noteViewportSize",viewport->size());card->raise();
}
void NotesController::syncSelection(){
    QSignalBlocker blocker(list);
    for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==window->document().activeAnnotation){list->setCurrentRow(i);return;}
    list->setCurrentRow(-1);
}
void NotesController::refresh(){
    auto &d=window->document();
    // Preserve text and item objects across playback. Only the current-moment filter depends on time.
    QString signature=d.id+":"+QString::number(d.metadataRevision)+":"+QString::number(d.structureRevision)+":"+QString::number(d.scene.fps)+":"+QString::number(d.scene.first)+":"+QString::number(d.scene.last)+":"+search->text()+":"+QString::number(filter->currentIndex());
    if(filter->currentIndex()==1){auto nodes=d.selection().values();std::sort(nodes.begin(),nodes.end());for(int n:nodes)signature+=":"+QString::number(n);}
    if(filter->currentIndex()==2){if(d.playing)return;signature+=":"+QString::number(d.frame);}
    if(signature!=listSignature){
        listSignature=signature;rebuilding=true;QSignalBlocker blocker(list);list->clear();
        for(auto value:d.scene.annotations){auto v=value.toObject();
            QString text=v["description"].toString(v["name"].toString());
            if(!(text+" "+v["name"].toString()).contains(search->text(),Qt::CaseInsensitive))continue;
            if(filter->currentIndex()==1){bool hit=false;for(auto n:v["nodes"].toArray())hit|=d.selection().contains(n.toInt());if(!hit)continue;}
            if(filter->currentIndex()==2 && !annotationActive(v,double(d.frame)/d.scene.fps,d.scene.fps))continue;
            auto summary=annotationSummary(d.scene,v);QString warnings;
            if(summary["outsideClip"].toBool())warnings+=" [Outside clip]";
            if(summary["missingBindings"].toBool())warnings+=" [Missing bone]";
            if(v.contains("attachment") && v["attachment"].toObject()["status"]!="attached")warnings+=" [Key unavailable]";
            auto item=new QListWidgetItem(annotationTimeLabel(v,d.scene.fps)+" | "+annotationBindings(d.scene,v)+warnings+"\n"+text.simplified().left(110),list);
            item->setData(Qt::UserRole,v["id"].toString());item->setToolTip(text.left(800));
        }
        rebuilding=false;
    }
    syncSelection();
    if(auto card=cards.value(d.id)){
        if(card->structure!=d.structureRevision){card->hide();card->deleteLater();cards.remove(d.id);}
        else {card->refreshAddress();if(card->property("noteViewportSize").toSize()!=window->viewport->size())placeCard(card);}
    }
}
void NotesController::removeSelected(){
    auto item=list->currentItem();if(!item)return;
    auto id=item->data(Qt::UserRole).toString();auto &d=window->document();
    try{EditService::execute(d,"motion_annotations",{{"operation","remove"},{"id",id}},"Delete note");
        if(auto card=cards.value(d.id))if(card->original["id"]==id){card->hide();card->deleteLater();}}
    catch(const std::exception &e){window->report(e.what(),true);}
}
} // namespace trickster
