#include "core/scene/workspace.h"
namespace trickster {
Workspace::Workspace(QObject *parent) : QObject(parent) {
    documents.push_back(create());
    history.setActiveStack(&active().undo);
}
std::shared_ptr<Document> Workspace::create(Scene scene, QString path) {
    auto d = std::make_shared<Document>();
    d->replace(std::move(scene), path);
    auto ptr = d.get();
    history.addStack(&ptr->undo);
    connect(ptr, &Document::changed, this, [this, ptr] {
        if (&active() == ptr)
            emit changed();
    });
    connect(ptr, &Document::selectionChanged, this, [this, ptr] {
        if (&active() == ptr)
            emit selectionChanged();
    });
    connect(ptr, &Document::sceneReplaced, this, [this, ptr] {
        if (&active() == ptr)
            emit sceneReplaced();
    });
    connect(ptr, &Document::feedback, this, [this, ptr](QString text) {
        if (&active() == ptr)
            emit feedback(text);
    });
    connect(&ptr->undo, &QUndoStack::indexChanged, this, [this, ptr] {
        if (&active() == ptr)
            emit historyChanged();
    });
    return d;
}
Document *Workspace::find(const QString &id) const {
    for (const auto &d : documents)
        if (d->id == id)
            return d.get();
    return nullptr;
}
void Workspace::activate(int index) {
    if (index < 0 || index >= int(documents.size()) || index == activeIndex)
        return;
    active().play(false);
    active().cancel();
    activeIndex = index;
    history.setActiveStack(&active().undo);
    emit activeChanged();
    emit sceneReplaced();
    emit selectionChanged();
    emit changed();
}
void Workspace::append(Scene scene, QString path, bool reuse) {
    active().play(false);
    active().cancel();
    if (reuse && active().scene.nodes.empty() && !active().dirty() && active().path.isEmpty()) {
        active().replace(std::move(scene), path);
        return;
    }
    documents.push_back(create(std::move(scene), path));
    activate(int(documents.size()) - 1);
}
void Workspace::remove(int index) {
    if (index < 0 || index >= int(documents.size()))
        return;
    auto removed = documents[index];
    removed->play(false);
    removed->cancel();
    if (documents.size() == 1)
        documents.push_back(create());
    documents.erase(documents.begin() + index);
    if (activeIndex > index)
        --activeIndex;
    else if (activeIndex == index)
        activeIndex = std::min(index, int(documents.size()) - 1);
    history.removeStack(&removed->undo);
    history.setActiveStack(&active().undo);
    emit activeChanged();
    emit sceneReplaced();
    emit selectionChanged();
    emit changed();
}
} // namespace trickster
