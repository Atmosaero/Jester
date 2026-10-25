#pragma once
#include "core/scene/document.h"
#include <QWidget>
#include <QImage>
class QPainter;
class AtomViewport;
#include <QPointF>
#include <QPointer>
#include <map>
#include <set>
#include <memory>
namespace trickster {
class Viewport : public QWidget {
    Q_OBJECT
  public:
    explicit Viewport(Document *document, QWidget *parent = nullptr);
    ~Viewport() override;
    enum Tool { Move, Rotate, Select };
    Tool tool = Move;
    QMap<int, std::vector<std::pair<double, QVector3D>>> motionPaths;
    quint64 motionPathRevision = 0;
    QString motionPathReferenceLabel;
    QMap<int, std::set<double>> motionPathBreaks;
    bool motionPathLabels = true, motionPathSpeedColors = false;
    double motionPathPastSeconds = -1, motionPathFutureSeconds = -1;
    QMap<int, QMap<double, double>> motionPathSpeeds;
    bool diagnosticOverlay = false, diagnosticTransparent = false;
    QJsonObject selectedMotionProblem;
    bool readOnly = false;
    bool forceCpuSkinning = false;
    QJsonObject renderingStats() const;
    QImage grabFramebuffer() const;
    QImage captureOverlay() const;
    QWidget* overlayWidget() const { return overlay; }
    bool isValid() const;
    float characterHeight() {
        return sceneHeight();
    }
    bool local = false, showMesh = true, showBones = true, showGhost = false, showTrajectory = false,
         showBalance = false;
    std::optional<Scene> physicsPreview;
    bool showPhysicsPreview = true, showPhysicsModel = false;
    QJsonObject physicsDiagnostics;
    struct Camera {
        float yaw = 24, pitch = 12, distance = 4.2f;
        QVector3D center{0, .8f, 0};
    };
    Camera camera() const {
        return {yaw, pitch, distance, center};
    }
    void setCamera(const Camera &value) {
        autoFramed = false;
        yaw = value.yaw;
        pitch = value.pitch;
        distance = value.distance;
        center = value.center;
        matrices();
        update();
    }
    bool showNotes = true;
    QPointF project(QVector3D p) const;
    std::optional<QPointF> projectVisible(QVector3D p) const;
  signals:
    void addNoteRequested();
    void annotationsPaint(QPainter *painter);
  public:
    void fit();
    void setDocument(Document *document);
    void viewPreset(int preset);
    bool isDragging() const {
        return dragging || selectingNodes;
    }
    void drawOverlay(QPainter &painter);

  protected:
    bool event(QEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

  private:
    AtomViewport* renderer = nullptr;
    QTimer* renderSyncTimer = nullptr;
    void clearTextures();
    bool autoFramed = true;
    float cachedRestHeight = -1;
    float sceneHeight();
    void syncRenderer();
    std::vector<QVector3D> trajectoryCache;
    int trajectoryNode = -1;
    void invalidateTrajectory();
    QWidget *overlay = nullptr;
    Document *doc;
    QPointer<Document> paintedDocument;
    quint64 paintedSession = 0, paintedOrdinal = 0;
    int hovered = -1;
    std::vector<int> pickCandidates(QPointF position) const;
    float yaw = 24, pitch = 12, distance = 4.2f;
    QVector3D center{0, .95f, 0};
    QMatrix4x4 view, projection;
    QPointF lastMouse, pressMouse;
    bool dragging = false, poleDrag = false, selectingNodes = false, selectionAdd = false;
    QSet<int> dragNodes;
    QRectF selectionRect;
    int axis = -1;
    Pose dragPose;
    QMap<int, Pin> dragPins;
    QVector3D dragOrigin, dragPole;
    QVector3D cameraRight() const;
    QVector3D cameraUp() const;
    QVector3D axisVector(int index) const;
    float scaleAt(QVector3D p) const;
    void matrices();
};
} // namespace trickster

namespace trickster { QImage captureEditorWidget(QWidget* widget); }
