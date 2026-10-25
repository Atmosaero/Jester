#pragma once
#include "document.h"
#include <QUndoGroup>
#include <memory>
namespace trickster {
// Persistent documents. Switching changes only the active pointer and view bindings.
class Workspace : public QObject {
    Q_OBJECT
  public:
    explicit Workspace(QObject *parent = nullptr);
    std::vector<std::shared_ptr<Document>> documents;
    QUndoGroup history;
    int activeIndex = 0;
    Document &active() const {
        return *documents.at(activeIndex);
    }
    Document *find(const QString &id) const;
    void activate(int index);
    void append(Scene scene, QString path = {}, bool reuse = true);
    void remove(int index);
  signals:
    void activeChanged();
    void changed();
    void selectionChanged();
    void sceneReplaced();
    void feedback(QString message);
    void historyChanged();

  private:
    std::shared_ptr<Document> create(Scene scene = {}, QString path = {});
};
} // namespace trickster
