#pragma once
#include "core.h"
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUndoStack>
#include <algorithm>
#include <functional>
#include <optional>

namespace trickster {
struct State {
    Pose pose;
    QMap<int, Pin> pins;
    QMap<int, Key> keys;
    int first, last, fps, frame;
    bool assist;
    QMap<int, float> limits;
    std::vector<Contact> contacts;
    QString name;
    MorphWeights morphs;
    PhysicsSettings physics;
    std::vector<RigidSegment> bodies;
    std::vector<MotionCorrection> layers;
    int activeLayer;
    bool compareOriginal;
    std::shared_ptr<const std::vector<AnimationClip>> clips;
    int sourceClip = -1;
    CharacterProfile profile;
    QMap<int, JointLimit> jointLimits;
    QJsonObject physicsResult;
    QJsonArray annotations;
    std::vector<ScalarCurve> curves;
    Pose curveBasePose;
    MorphWeights curveBaseMorphs;
    QJsonObject retargetProfile;
    QMap<int, Pin> curveBasePins;
};
struct HistoryEntry {
    State before, after;
    QString label;
    bool metadataOnly = false;
};
struct SceneSession {
    Scene scene;
    QString path;
    int frame = 0, selected = -1, index = 0, cleanIndex = 0;
    std::vector<HistoryEntry> history;
    QSet<int> selection;
};
class Document : public QObject {
    Q_OBJECT
  public:
    explicit Document(QObject *parent = nullptr);
    const QString id;
    quint64 revision = 0, motionRevision = 0, metadataRevision = 0;
    quint64 structureRevision = 0, meshRevision = 0, profileRevision = 0, limitsRevision = 0,
            materialRevision = 0;
    Scene scene;
    bool navigatingAnnotation = false;
    QString activeAnnotation; // ephemeral shared selection, not authored metadata
    void selectAnnotation(const QString &id, bool navigate = true);
    QJsonArray motionProblems; // ephemeral immutable analysis, never authored animation
    quint64 analysisRevision = 0, analysisMotionRevision = 0, analysisMetadataRevision = 0;
    std::optional<QPair<double, double>> timeSelection;
    void selectTime(double a, double b) {
        timeSelection = qMakePair(std::min(a, b), std::max(a, b));
        emit changed();
    }
    QUndoStack undo;
    QString path;
    int selected = -1, frame = 0;
    bool playing = false, loop = true, symmetryEditing = false;
    quint64 playbackSession = 0, playbackOrdinal = 0, presentedPlaybackFrames = 0;
    QJsonObject playbackStats() const;
    void markFramePresented(quint64 session, quint64 ordinal);
    bool dirty() const {
        return !undo.isClean();
    }
    bool inTransaction() const {
        return before.has_value();
    }
    State state() const;
    SceneSession captureSession() const;
    void restoreSession(const SceneSession &session);
    void apply(const State &s, bool metadataOnly = false);
    void replace(Scene s, QString file = {});
    void select(int node, bool toggle = false);
    void setSelection(QSet<int> nodes, int primary = -1);
    QSet<int> selection() const;
    void setSymmetry(bool enabled);
    QString recordingContext() const;
    void seek(int value);
    void play(bool enable);
    void begin();
    void commit(const QString &label, bool poseChanged = true, bool metadataOnly = false);
    void cancel();
    void edit(const QString &label, const std::function<void(Scene &)> &action, bool poseChanged = true,
              bool metadataOnly = false);
    void insertKey();
    void removeKey(int at, int node = -1, int channels = 7);
    bool moveKey(int from, int to, bool copy = false, int node = -1, int channels = 7);
    void togglePin();
    void resetPose();
    void pinFeet();
    void demo();
  signals:
    void annotationSelected(QString id);
    void changed();
    void selectionChanged();
    void sceneReplaced();
    void feedback(QString message);

  private:
    QSet<int> selectedNodes;
    std::optional<State> before;
    QTimer timer;
    QElapsedTimer elapsed;
    int startFrame = 0;
    qint64 lastPresentedOrdinal = -1, playbackElapsedMs = 0;
};
} // namespace trickster
