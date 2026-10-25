#include "core/scene/document.h"
#include "core/scene/edit_service.h"
#include "core/annotations/motion_annotations.h"
#include <QSignalBlocker>
#include <QUndoCommand>
#include <QUuid>
#include <algorithm>
#include <cmath>
namespace trickster {
static void trackDependencies(Document &document, const State &previous, const State &s, bool metadataOnly) {
    auto sameProfile = [](const CharacterProfile &a, const CharacterProfile &b) {
        if (a.configured != b.configured || a.roles != b.roles || a.mirrors != b.mirrors ||
            a.chains.keys() != b.chains.keys())
            return false;
        for (auto it = a.chains.begin(); it != a.chains.end(); ++it) {
            auto &x = it.value();
            auto y = b.chains.value(it.key());
            if (x.root != y.root || x.mid != y.mid || x.end != y.end || x.pole != y.pole)
                return false;
        }
        return true;
    };
    if (!sameProfile(previous.profile, s.profile))
        ++document.profileRevision;
    bool changedLimits = previous.limits != s.limits || previous.jointLimits.keys() != s.jointLimits.keys();
    if (!changedLimits)
        for (auto it = previous.jointLimits.begin(); it != previous.jointLimits.end(); ++it) {
            auto &a = it.value();
            auto b = s.jointLimits.value(it.key());
            if (a.hinge != b.hinge || a.axis != b.axis || a.swing != b.swing || a.minimum != b.minimum ||
                a.maximum != b.maximum) {
                changedLimits = true;
                break;
            }
        }
    if (changedLimits)
        ++document.limitsRevision;
    if (!metadataOnly && previous.annotations != s.annotations)
        ++document.metadataRevision;
}
class StateCommand : public QUndoCommand {
    Document *doc;
    State a, b;
    bool skipFirst = false, metadataOnly = false;

  public:
    StateCommand(Document *d, State old, State next, QString label, bool skip = false, bool metadata = false)
        : QUndoCommand(label), doc(d), a(std::move(old)), b(std::move(next)), skipFirst(skip),
          metadataOnly(metadata) {}
    HistoryEntry entry() const {
        return {a, b, text(), metadataOnly};
    }
    void undo() override {
        doc->apply(a, metadataOnly);
    }
    void redo() override {
        if (skipFirst) {
            skipFirst = false;
            return;
        }
        doc->apply(b, metadataOnly);
    }
};
Document::Document(QObject *parent)
    : QObject(parent), id(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    connect(&undo, &QUndoStack::indexChanged, this, [this] { ++revision; });
    undo.setUndoLimit(100);
    timer.setInterval(8);
    connect(&timer, &QTimer::timeout, this, [this] {
        int next = startFrame + int(elapsed.elapsed() * scene.fps / 1000);
        playbackOrdinal = quint64(std::max(0, next - startFrame));
        playbackElapsedMs = elapsed.elapsed();
        if (next > scene.last) {
            if (loop) {
                next = scene.first + (next - scene.first) % (scene.last - scene.first + 1);
            } else {
                playbackOrdinal = quint64(std::max(0, scene.last - startFrame));
                seek(scene.last);
                play(false);
                return;
            }
        }
        seek(next);
    });
    connect(&undo, &QUndoStack::cleanChanged, this, [this] { emit changed(); });
}
State Document::state() const {
    return {scene.pose,
            scene.pins,
            scene.keys,
            scene.first,
            scene.last,
            scene.fps,
            frame,
            scene.bodyAssist,
            scene.rotationLimits,
            scene.contacts,
            scene.name,
            scene.morphs,
            scene.physics,
            scene.bodies,
            scene.layers,
            scene.activeLayer,
            scene.compareOriginal,
            scene.clips,
            scene.sourceClip,
            scene.profile,
            scene.jointLimits,
            scene.physicsResult,
            scene.annotations,
            scene.curves,
            scene.curveBasePose,
            scene.curveBaseMorphs,
            scene.retargetProfile,
            scene.curveBasePins};
}
SceneSession Document::captureSession() const {
    SceneSession result;
    result.scene = scene;
    result.path = path;
    result.frame = frame;
    result.selected = selected;
    result.selection = selection();
    result.index = undo.index();
    result.cleanIndex = undo.cleanIndex();
    for (int i = 0; i < undo.count(); ++i)
        if (auto command = dynamic_cast<const StateCommand *>(undo.command(i)))
            result.history.push_back(command->entry());
    return result;
}
void Document::restoreSession(const SceneSession &session) {
    ++motionRevision;
    ++metadataRevision;
    ++structureRevision;
    ++meshRevision;
    ++profileRevision;
    ++limitsRevision;
    ++materialRevision;
    activeAnnotation.clear();
    timeSelection.reset();
    playbackSession = playbackOrdinal = presentedPlaybackFrames = 0;
    lastPresentedOrdinal = -1;
    playbackElapsedMs = 0;
    {
        QSignalBlocker blocker(this);
        play(false);
        before.reset();
        scene = session.scene;
        path = session.path;
        selected = session.selected;
        selectedNodes = session.selection;
        if (selected >= 0)
            selectedNodes.insert(selected);
        frame = session.frame;
        undo.clear();
        for (const auto &entry : session.history)
            undo.push(
                new StateCommand(this, entry.before, entry.after, entry.label, true, entry.metadataOnly));
        if (session.cleanIndex < 0)
            undo.resetClean();
        else {
            undo.setIndex(session.cleanIndex);
            undo.setClean();
        }
        undo.setIndex(session.index);
        scene = session.scene;
        frame = session.frame;
        selected = session.selected;
        selectedNodes = session.selection;
        if (selected >= 0)
            selectedNodes.insert(selected);
        path = session.path;
    }
    emit sceneReplaced();
    emit selectionChanged();
    emit changed();
}
void Document::apply(const State &s, bool metadataOnly) {
    trackDependencies(*this, state(), s, metadataOnly);
    if (metadataOnly)
        ++metadataRevision;
    else
        ++motionRevision;
    if (metadataOnly) {
        scene.annotations = s.annotations;
        // Explicit attachments may assign stable bundle IDs, but metadata Undo never rewinds
        // the current frame, pose, keys, layers or render buffers after a later seek.
        for (auto it=scene.keys.begin(); it!=scene.keys.end(); ++it)
            if (s.keys.contains(it.key())) it->id=s.keys[it.key()].id;
        for (auto &layer:scene.layers)
            for (const auto &old:s.layers) if (old.id==layer.id)
                for (auto it=layer.keys.begin();it!=layer.keys.end();++it)
                    if(old.keys.contains(it.key()))it->id=old.keys[it.key()].id;
        emit changed();
        return;
    }
    scene.retargetProfile = s.retargetProfile;
    scene.curves = s.curves;
    scene.curveBasePose = s.curveBasePose;
    scene.curveBaseMorphs = s.curveBaseMorphs;
    scene.curveBasePins = s.curveBasePins;
    scene.annotations = s.annotations;
    scene.physicsResult = s.physicsResult;
    scene.jointLimits = s.jointLimits;
    scene.profile = s.profile;
    scene.clips = s.clips;
    scene.sourceClip = s.sourceClip;
    scene.layers = s.layers;
    scene.activeLayer = s.activeLayer;
    scene.compareOriginal = s.compareOriginal;
    scene.pose = s.pose;
    scene.morphs = s.morphs;
    scene.physics = s.physics;
    scene.bodies = s.bodies;
    scene.pins = s.pins;
    scene.keys = s.keys;
    scene.first = s.first;
    scene.last = s.last;
    scene.fps = s.fps;
    frame = s.frame;
    scene.sampleFrame = float(frame);
    scene.bodyAssist = s.assist;
    scene.rotationLimits = s.limits;
    scene.contacts = s.contacts;
    scene.name = s.name;
    emit changed();
}
void Document::replace(Scene s, QString file) {
    s.posingRestHeight=s.height();
    motionProblems={}; ++analysisRevision;
    ++revision;
    ++motionRevision;
    ++metadataRevision;
    ++structureRevision;
    ++meshRevision;
    ++profileRevision;
    ++limitsRevision;
    ++materialRevision;
    activeAnnotation.clear();
    timeSelection.reset();
    playbackSession = playbackOrdinal = presentedPlaybackFrames = 0;
    lastPresentedOrdinal = -1;
    playbackElapsedMs = 0;
    play(false);
    before.reset();
    scene = std::move(s);
    path = file;
    frame = std::clamp(int(std::round(scene.sampleFrame)), scene.first, scene.last);
    scene.sampleFrame = float(frame);
    selected = scene.nodes.empty() ? -1 : 0;
    selectedNodes.clear();
    if (selected >= 0)
        selectedNodes.insert(selected);
    undo.clear();
    undo.setClean();
    emit sceneReplaced();
    emit selectionChanged();
    emit changed();
    emit feedback(scene.nodes.empty() ? "Empty scene. Open a character or choose a scene."
                                      : "Scene ready. Motion: correct an interval; W/E pose, Space play.");
}
QSet<int> Document::selection() const {
    auto value = selectedNodes;
    if (selected >= 0 && selected < int(scene.nodes.size()))
        value.insert(selected);
    return value;
}
void Document::setSelection(QSet<int> nodes, int primary) {
    for (int n : nodes)
        if (n < 0 || n >= int(scene.nodes.size()))
            return;
    if (!nodes.contains(primary))
        primary = nodes.isEmpty() ? -1 : *std::min_element(nodes.begin(), nodes.end());
    if (primary == selected && nodes == selectedNodes)
        return;
    if (before)
        cancel();
    selected = primary;
    selectedNodes = nodes;
    emit selectionChanged();
    emit changed();
}
void Document::select(int n, bool toggle) {
    if (n < -1 || n >= int(scene.nodes.size()))
        return;
    if (!toggle) {
        setSelection(n < 0 ? QSet<int>{} : QSet<int>{n}, n);
        return;
    }
    auto nodes = selection();
    if (nodes.contains(n))
        nodes.remove(n);
    else if (n >= 0)
        nodes.insert(n);
    setSelection(nodes, nodes.contains(n) ? n : selected);
}
void Document::setSymmetry(bool enabled) {
    if (before)
        cancel();
    symmetryEditing = enabled;
    emit changed();
    if (enabled && scene.mirrorPartner(selected) < 0)
        emit feedback("No mirror partner assigned. Review Character profile > Mirror pairs.");
}
QString Document::recordingContext() const {
    const auto &c = scene.correction();
    QString source =
        scene.sourceClip >= 0 && scene.clips ? (*scene.clips)[scene.sourceClip].name : QString("Pose keys");
    if (scene.compareOriginal)
        return "Viewing source: " + source + " | Recording OFF - turn off Compare to edit";
    if (c.present)
        return (c.enabled && c.editing ? QString("Record: ") : QString("Locked: ")) + c.name + " | " +
               (c.world ? "World offsets" : "Local offsets") + " | Source: " + source;
    if (scene.sourceClip >= 0)
        return "Source: " + source + " (read-only) | First edit creates a correction layer";
    return "Record: whole-pose keys | TRS + pins + morphs";
}
void Document::seek(int f) {
    f = std::clamp(f, scene.first, scene.last);
    if (f == frame)
        return;
    if (before)
        cancel();
    frame = f;
    scene.evaluate(float(frame));
    emit changed();
}
void Document::play(bool on) {
    if (on && scene.nodes.empty()) {
        emit feedback("Add a character before playback.");
        return;
    }
    if (on == playing)
        return;
    if (on && before)
        cancel();
    playing = on;
    if (on) {
        if (frame == scene.last)
            seek(scene.first);
        startFrame = frame;
        ++playbackSession;
        playbackOrdinal = presentedPlaybackFrames = 0;
        lastPresentedOrdinal = -1;
        playbackElapsedMs = 0;
        elapsed.restart();
        timer.start();
    } else {
        playbackElapsedMs = elapsed.elapsed();
        timer.stop();
    }
    emit changed();
}
QJsonObject Document::playbackStats() const {
    quint64 due = playbackSession ? playbackOrdinal + 1 : 0;
    return {{"session", qint64(playbackSession)},
            {"active", playing},
            {"logicalFramesDue", qint64(due)},
            {"presentedFrames", qint64(presentedPlaybackFrames)},
            {"droppedFrames", qint64(due > presentedPlaybackFrames ? due - presentedPlaybackFrames : 0)},
            {"elapsedMs", playing ? double(elapsed.elapsed()) : double(playbackElapsedMs)},
            {"presentationMethod", "Unique playback ordinals acknowledged by Atom RPI frame preparation; "
                                   "render submission, not a physical display latency measurement"}};
}
void Document::markFramePresented(quint64 session, quint64 ordinal) {
    if (session != playbackSession || !session || qint64(ordinal) <= lastPresentedOrdinal)
        return;
    lastPresentedOrdinal = qint64(ordinal);
    ++presentedPlaybackFrames;
}
void Document::begin() {
    play(false);
    if (!before)
        before = state();
}
void Document::commit(const QString &label, bool poseChanged, bool metadataOnly) {
    if (!before)
        return;
    scene.sampleFrame = float(frame);
    if (poseChanged) {
        if (before->compareOriginal ||
            (scene.correction().present && (!scene.correction().enabled || !scene.correction().editing))) {
            cancel();
            emit feedback(
                "Recording is off. Leave source comparison and enable the active layer's Record toggle.");
            return;
        }
        bool curveDrivenLayer=scene.correction().present&&std::any_of(scene.curves.begin(),scene.curves.end(),[&](const auto &curve){return curve.layer==scene.correction().id;});
        if (((scene.sourceClip >= 0 || !scene.curves.empty()) && !scene.correction().present)||curveDrivenLayer) {
            QString error;
            int fade = std::max(1, scene.fps / 3);
            if (!scene.beginCorrection(scene.first, scene.last, std::min(fade, frame - scene.first),
                                       std::min(fade, scene.last - frame), false, error)) {
                cancel();
                emit feedback(error);
                return;
            }
        }
        if (scene.correction().present && scene.correction().editing) {
            QString error;
            if (!scene.captureCorrection(before->pose, before->morphs, error, &before->pins)) {
                cancel();
                emit feedback(error);
                return;
            }
        } else {
            if (scene.correction().present) {
                cancel();
                emit feedback("Enable Edit correction, or bake a new scene before editing source keys.");
                return;
            }
            scene.sourceClip = -1; // Explicit source-key editing uses the editable sampled keys.
            auto id = scene.keys.value(frame).id;
            scene.keys[frame] = {scene.pose, scene.pins, scene.morphs, id};
        }
    }
    synchronizeAnnotationAttachments(scene, before->fps != scene.fps);
    QString validationError;
    if (!scene.validate(&validationError)) {
        cancel();
        emit feedback(validationError);
        return;
    }
    if (!metadataOnly && !scene.physicsResult.isEmpty() && scene.physicsResult == before->physicsResult &&
        label != "Physics result")
        scene.physicsResult["validationStale"] = true;
    State old = *before;
    trackDependencies(*this, old, state(), metadataOnly);
    before.reset();
    undo.push(new StateCommand(this, std::move(old), state(), label, false, metadataOnly));
    emit feedback(label + (poseChanged ? QString(scene.correction().present && scene.correction().editing
                                                     ? " · correction at frame %1"
                                                     : " · key at frame %1")
                                             .arg(frame)
                                       : QString()));
}
void Document::cancel() {
    if (before) {
        State s = *before;
        before.reset();
        apply(s);
    }
}
void Document::edit(const QString &label, const std::function<void(Scene &)> &action, bool poseChanged,
                    bool metadataOnly) {
    if (poseChanged && scene.nodes.empty()) {
        emit feedback("Choose a scene or open a character first.");
        return;
    }
    begin();
    action(scene);
    commit(label, poseChanged, metadataOnly);
}
void Document::insertKey() {
    if (scene.correction().present || scene.sourceClip >= 0) {
        try {
            EditService::execute(*this, "timeline", {{"operation", "insert"}}, "Insert correction key");
        } catch (const std::exception &e) {
            emit feedback(QString::fromUtf8(e.what()));
        }
        return;
    }
    edit("Insert pose key", [](Scene &) {}, true);
}
void Document::removeKey(int at, int node, int channels) {
    if (scene.correction().present) {
        try {
            QJsonObject arguments{{"operation", "key_delete"}, {"from", at}, {"channels", channels}};
            if (node >= 0)
                arguments["node"] = node;
            EditService::execute(*this, "motion_correction", arguments, "Delete correction key");
        } catch (const std::exception &e) {
            emit feedback(QString::fromUtf8(e.what()));
        }
        return;
    }
    if (scene.sourceClip >= 0) {
        emit feedback("Source clip timing is read-only. Bake as new scene to edit pose keys.");
        return;
    }
    if (!scene.keys.contains(at))
        return;
    edit(
        "Delete key",
        [&](Scene &s) {
            s.keys.remove(at);
            s.evaluate(float(frame));
        },
        false);
}
bool Document::moveKey(int from, int to, bool copy, int node, int channels) {
    if (scene.correction().present) {
        try {
            QJsonObject arguments{{"operation", copy ? "key_copy" : "key_move"},
                                  {"from", from},
                                  {"frame", to},
                                  {"channels", channels}};
            if (node >= 0)
                arguments["node"] = node;
            EditService::execute(*this, "motion_correction", arguments,
                                 copy ? "Copy correction key" : "Move correction key");
            seek(to);
            return true;
        } catch (const std::exception &e) {
            emit feedback(QString::fromUtf8(e.what()));
            return false;
        }
    }
    if (scene.sourceClip >= 0 || from == to || !scene.keys.contains(from) || to < scene.first ||
        to > scene.last)
        return false;
    if (scene.keys.contains(to)) {
        emit feedback("A key already exists at that frame. Choose an empty frame.");
        return false;
    }
    edit(
        copy ? "Copy key" : "Move key",
        [&](Scene &s) {
            auto key = s.keys[from];
            if (!copy)
                s.keys.remove(from);
            s.keys[to] = key;
            if (copy)
                s.keys[to].id.clear();
            frame = to;
            s.evaluate(float(to));
        },
        false);
    return true;
}
void Document::togglePin() {
    QSet<int> effectors;
    bool pin = false;
    for (int n : selection())
        if (scene.isEffector(n)) {
            effectors.insert(n);
            pin |= !scene.pins.contains(n);
        }
    if (effectors.isEmpty())
        return;
    edit(pin ? "Pin selected effectors" : "Release selected effectors", [&](Scene &s) {
        auto positions = s.positions(s.pose);
        for (int n : effectors)
            if (pin)
                s.pins[n] = {positions[n], s.defaultPole(n), 1};
            else
                s.pins.remove(n);
    });
}
void Document::resetPose() {
    edit("Reset pose", [](Scene &s) {
        s.pose = s.restPose();
        s.pins.clear();
    });
}
void Document::pinFeet() {
    bool found = false;
    for (int i = 0; i < int(scene.nodes.size()); ++i)
        if (scene.isFoot(i) && scene.isEffector(i))
            found = true;
    if (!found) {
        emit feedback("Assign foot roles and IK chains in Motion → Character profile.");
        return;
    }
    edit("Pin both feet", [](Scene &s) {
        auto p = s.positions(s.pose);
        for (int i = 0; i < int(s.nodes.size()); ++i)
            if (s.isFoot(i) && s.isEffector(i))
                s.pins[i] = {p[i], s.defaultPole(i), 1};
    });
}
void Document::demo() {
    Scene s = Scene::mannequin();
    auto p = s.positions(s.pose);
    for (int foot : {15, 19})
        s.pins[foot] = {p[foot], s.defaultPole(foot), 1};
    s.keys[0] = {s.pose, s.pins, s.morphs};
    s.translateWorld(0, {0, -.18f, .03f});
    s.solveIK(8, p[8] + QVector3D(-.16f, .21f, .25f), s.defaultPole(8));
    s.keys[30] = {s.pose, s.pins, s.morphs};
    s.translateWorld(0, {0, .08f, -.02f});
    s.solveIK(8, p[8] + QVector3D(-.28f, .35f, .12f), s.defaultPole(8));
    s.solveIK(12, p[12] + QVector3D(.1f, .12f, .22f), s.defaultPole(12));
    s.keys[60] = {s.pose, s.pins, s.morphs};
    s.keys[90] = s.keys[0];
    s.evaluate(0);
    replace(std::move(s));
    emit feedback("Demo: four poses, pinned feet and hand IK. Space to play.");
}
} // namespace trickster
