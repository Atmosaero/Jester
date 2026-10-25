#include "ui/notes/notes_overlay.h"
#include "ui/notes/notes_ui.h"
#include "core/annotations/motion_annotations.h"
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QTextDocument>
#include <cmath>
namespace trickster {
NotesOverlay::NotesOverlay(Viewport *v,NotesController *c):QObject(c),view(v),controller(c){
    view->installEventFilter(this);
    connect(view,&Viewport::annotationsPaint,this,&NotesOverlay::draw);
}
void NotesOverlay::bind(Document *d){document=d;hits.clear();hovered.clear();view->update();}
void NotesOverlay::draw(QPainter *p){
    hits.clear();QVariantMap exposed;
    if(!document || document->playing || !view->showNotes){view->setProperty("annotationMarkers",exposed);return;}
    const auto &s=document->scene;
    if(s.annotations.empty()){view->setProperty("annotationMarkers",exposed);return;}
    auto positions=s.positions(s.pose);
    QRectF bounds=view->rect().adjusted(44,std::min(48,view->height()/5),-12,-std::min(62,view->height()/4));
    if(bounds.width()<30 || bounds.height()<30){view->setProperty("annotationMarkers",exposed);return;}
    QPointF gizmo(-1000,-1000);
    if(document->selected>=0 && document->selected<int(positions.size()))
        if(auto point=view->projectVisible(positions[document->selected]))gizmo=*point;
    for(auto value:s.annotations){
        auto note=value.toObject();
        if(!annotationActive(note,double(document->frame)/s.fps,s.fps))continue;
        QPointF anchor(bounds.left()+12,bounds.top()+12);
        auto nodes=note["nodes"].toArray();
        if(!nodes.empty()){
            // The first authored bone is the semantic primary anchor, never a screen coordinate.
            int node=nodes[0].toInt(-1);
            if(node<0 || node>=int(positions.size()))continue;
            auto screen=view->projectVisible(positions[node]);if(!screen)continue;anchor=*screen;
        }
        QRectF marker;
        bool placed=false;
        for(int step=0;step<96;++step){
            double angle=(step%12)*6.28318530718/12.;double radius=34+(step/12)*29;
            QPointF center=anchor+QPointF(std::cos(angle)*radius,std::sin(angle)*radius);
            center.setX(std::clamp(center.x(),bounds.left()+13,bounds.right()-13));
            center.setY(std::clamp(center.y(),bounds.top()+13,bounds.bottom()-13));
            marker=QRectF(center-QPointF(13,12),QSizeF(26,24));
            if(view->tool!=Viewport::Select && QLineF(center,gizmo).length()<88)continue;
            bool collision=false;for(auto rect:hits)if(rect.adjusted(-3,-3,3,3).intersects(marker)){collision=true;break;}
            if(!collision){placed=true;break;}
        }
        if(!placed)continue; // Crowded/offscreen notes remain navigable in the dock.
        QString id=note["id"].toString();hits[id]=marker;exposed[id]=marker.toRect();
        p->save();p->setPen(QPen(QColor(240,192,90,150),1));p->drawLine(anchor,marker.center());
        p->setPen(QPen(QColor(245,205,120),id==document->activeAnnotation?2:1));
        p->setBrush(id==document->activeAnnotation?QColor(160,106,30):QColor(50,46,37,235));
        p->drawRoundedRect(marker,5,5);
        p->drawLine(marker.topLeft()+QPointF(6,8),marker.topRight()+QPointF(-6,8));
        p->drawLine(marker.topLeft()+QPointF(6,13),marker.topRight()+QPointF(-8,13));
        if(nodes.empty())p->drawText(marker,Qt::AlignCenter,"C");
        p->restore();
    }
    view->setProperty("annotationMarkers",exposed);
}
bool NotesOverlay::eventFilter(QObject *,QEvent *event){
    if(event->type()==QEvent::Resize){QTimer::singleShot(0,controller,[this]{controller->refresh();});return false;}
    if(!document || document->playing || !view->showNotes)return false;
    if(event->type()==QEvent::MouseButtonPress){auto mouse=static_cast<QMouseEvent*>(event);
        if(mouse->button()==Qt::LeftButton && mouse->modifiers()==Qt::NoModifier)
            for(auto it=hits.begin();it!=hits.end();++it)if(it.value().contains(mouse->position())){
                document->selectAnnotation(it.key());event->accept();return true;}
    }
    if(event->type()==QEvent::ToolTip){auto help=static_cast<QHelpEvent*>(event);
        for(auto it=hits.begin();it!=hits.end();++it)if(it.value().contains(help->pos())){
            for(auto value:document->scene.annotations){auto note=value.toObject();if(note["id"]==it.key()){
                QString text=annotationBindings(document->scene,note)+" | "+annotationTimeLabel(note,document->scene.fps)+"\n"+note["description"].toString(note["name"].toString()).left(240);
                QToolTip::showText(help->globalPos(),Qt::convertFromPlainText(text),view);return true;}}
        }
    }
    return false;
}
} // namespace trickster
