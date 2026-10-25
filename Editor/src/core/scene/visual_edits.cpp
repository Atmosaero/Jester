#include "core/scene/document.h"
namespace trickster {
namespace {
class VisualCommand final:public DocumentHistoryCommand {
    Document *document;
    SceneVisualSettings before,after;
    int gesture;
  public:
    VisualCommand(Document *doc,SceneVisualSettings old,SceneVisualSettings next,const QString &label,int token)
        :DocumentHistoryCommand(label),document(doc),before(std::move(old)),after(std::move(next)),gesture(token) {}
    int id() const override {return gesture?0x4a5356:-1;}
    bool mergeWith(const QUndoCommand *command) override {
        auto *other=dynamic_cast<const VisualCommand*>(command);
        if(!other || !gesture || gesture!=other->gesture || document!=other->document)return false;
        after=other->after;return true;
    }
    void undo() override {document->applyVisual(before);}
    void redo() override {document->applyVisual(after);}
    HistoryEntry entry() const override {
        State a{},b{};a.visuals=before;b.visuals=after;
        return {std::move(a),std::move(b),text(),false,true};
    }
};
}
bool Document::editVisual(const QString &label,const SceneVisualSettings &settings,int mergeToken) {
    if(inTransaction()) {emit feedback("Finish the current animation edit before changing scene settings.");return false;}
    if(scene.visuals==settings)return true;
    undo.push(new VisualCommand(this,scene.visuals,settings,label,mergeToken));
    emit feedback(label);return true;
}
void Document::applyVisual(const SceneVisualSettings &settings) {
    if(scene.visuals==settings)return;
    scene.visuals=settings;
    ++visualRevision;
    emit changed();
}
} // namespace trickster
