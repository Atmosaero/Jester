#pragma once
#include "core/scene/workspace.h"
#include <QDockWidget>
#include <QPointer>
class QLineEdit;
class QComboBox;
class QListWidget;
namespace trickster {
class Window;
class NotesCard;
class NotesOverlay;
class NotesController : public QObject {
    Q_OBJECT
  public:
    explicit NotesController(Window *);
    ~NotesController() override;
    void addNote();
    void openNote(const QString &);
    void showPanel();
    void refresh();
    void placeCard(NotesCard *);
  private:
    Window *window;
    QDockWidget *dock;
    QLineEdit *search;
    QComboBox *filter;
    QListWidget *list;
    NotesOverlay *overlay;
    QPointer<Document> bound;
    QMap<QString, QPointer<NotesCard>> cards;
    QString listSignature;
    bool rebuilding = false;
    void bind();
    void syncSelection();
    void removeSelected();
};
} // namespace trickster
