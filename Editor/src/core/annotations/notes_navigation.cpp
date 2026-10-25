#include "core/scene/document.h"
#include "core/annotations/motion_annotations.h"
#include <QScopedValueRollback>
namespace trickster {
void Document::selectAnnotation(const QString &annotationId, bool navigate) {
    for (auto value : scene.annotations) {
        auto note = value.toObject();
        if (note["id"] != annotationId) continue;
        QScopedValueRollback<bool> navigation(navigatingAnnotation, true);
        activeAnnotation = annotationId;
        if (navigate) {
            play(false);
            seek(std::clamp(annotationFrame(note["start"].toDouble(), scene.fps), scene.first, scene.last));
            QSet<int> bones;
            for (auto n : note["nodes"].toArray())
                if (n.toInt(-1) >= 0 && n.toInt() < int(scene.nodes.size())) bones.insert(n.toInt());
            setSelection(bones);
        }
        emit annotationSelected(annotationId);
        emit changed();
        return;
    }
}
}
