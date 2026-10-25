#pragma once
#include "viewport.h"
#include <QObject>
namespace trickster {
class NotesController;
class NotesOverlay : public QObject {
    Viewport *view;
    NotesController *controller;
    Document *document = nullptr;
    QMap<QString, QRectF> hits;
    QString hovered;
  public:
    NotesOverlay(Viewport *, NotesController *);
    void bind(Document *);
    void draw(QPainter *);
  protected:
    bool eventFilter(QObject *, QEvent *) override;
};
} // namespace trickster
