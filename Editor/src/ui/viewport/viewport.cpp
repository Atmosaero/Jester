#include "ui/viewport/viewport.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include "render/atom/AtomViewport.h"
#include <QTimer>
#include <QCoreApplication>
#include <QApplication>
#include <QScreen>
#include <QCursor>
#ifdef Q_OS_WIN
#include <Windows.h>
#endif
namespace trickster {
namespace {
float segmentDistance(QPointF p, QPointF a, QPointF b) {
    auto d = b - a;
    double len = d.x() * d.x() + d.y() * d.y();
    double t = len > 0 ? std::clamp(QPointF::dotProduct(p - a, d) / len, 0., 1.) : 0;
    return float(QLineF(p, a + d * t).length());
}
} // namespace
class ViewOverlay : public QWidget {
    Viewport *view;

  public:
    explicit ViewOverlay(Viewport *v) : QWidget(v,Qt::Tool | Qt::FramelessWindowHint), view(v) {
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_QuitOnClose,false);
        setAttribute(Qt::WA_NoMousePropagation);
    }
    bool nativeEvent(const QByteArray& type,void* message,qintptr* result) override {
#ifdef Q_OS_WIN
        const auto* native=static_cast<const MSG*>(message);
        if(native->message==WM_NCHITTEST && !childAt(mapFromGlobal(QCursor::pos()))) {
            *result=HTTRANSPARENT;
            return true;
        }
#endif
        return QWidget::nativeEvent(type,message,result);
    }
    bool event(QEvent* event) override {
        // Card widgets receive ordinary Qt input. Background/marker events
        // continue to the viewport, including on platforms without native hit
        // testing and for synthetic accessibility/automation events.
        switch(event->type()) {
        case QEvent::MouseButtonPress:case QEvent::MouseButtonRelease:case QEvent::MouseMove:
        case QEvent::Wheel:case QEvent::ContextMenu:case QEvent::ToolTip:
            if(!childAt(mapFromGlobal(QCursor::pos()))) return QCoreApplication::sendEvent(view,event);
            break;
        default:break;
        }
        return QWidget::event(event);
    }
    void syncGeometry() {
        auto* modal=QApplication::activeModalWidget();
        bool display=view->isVisible() && !view->window()->isMinimized() && view->width()>0 && view->height()>0
            && (!modal || modal==view->window() || modal->isAncestorOf(view));
        if(!display) { hide();return; }
        const QRect target(view->mapToGlobal(QPoint()),view->size());
        if(geometry()!=target) setGeometry(target);
        if(!isVisible()) show();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(),Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        view->drawOverlay(painter);
    }
};
Viewport::Viewport(Document* d, QWidget* parent) : QWidget(parent), doc(nullptr) {
    setObjectName("viewport");
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320,110);
    setMouseTracking(true);
    renderer = new AtomViewport(this, false);
    renderer->externalEvents = [this](QEvent* event) { return QCoreApplication::sendEvent(this,event); };
    renderer->onFrame = [this] {
        if (paintedDocument) paintedDocument->markFramePresented(paintedSession,paintedOrdinal);
    };
    overlay = new ViewOverlay(this);
    overlay->raise();
    setDocument(d);
    auto* timer = new QTimer(this);
    renderSyncTimer=timer;
    connect(timer,&QTimer::timeout,this,[this] {
        static_cast<ViewOverlay*>(overlay)->syncGeometry();
        if (!isVisible() || !doc) return;
        syncRenderer();
        overlay->update();
    });
    timer->start(16);
    setToolTip("Right drag or Alt+Left: orbit | Middle or Shift+Right: pan | Wheel: zoom\nLeft: select / pose / IK | W/E: move/rotate | F: frame | Escape: cancel");
}
Viewport::~Viewport() {
    renderSyncTimer->stop();
    renderer->onFrame={};
    renderer->externalEvents={};
    delete overlay;overlay=nullptr;
    delete renderer;renderer=nullptr;
}
void Viewport::setDocument(Document *d) {
    diagnosticOverlay = false;
    diagnosticTransparent = false;
    selectedMotionProblem = {};
    motionPaths.clear();
    motionPathBreaks.clear();
    if (doc) {
        disconnect(doc, nullptr, this, nullptr);
        disconnect(&doc->undo, nullptr, this, nullptr);
    }
    doc = d;
    hovered = -1;
    connect(d, &Document::changed, this, QOverload<>::of(&Viewport::update));
    connect(d, &Document::sceneReplaced, this, &Viewport::fit);
    connect(d, &Document::sceneReplaced, this, &Viewport::clearTextures);
    connect(d, &Document::sceneReplaced, this, &Viewport::invalidateTrajectory);
    connect(&d->undo, &QUndoStack::indexChanged, this, [this, revision=d->motionRevision]() mutable {
        if (revision != doc->motionRevision) { revision=doc->motionRevision; invalidateTrajectory(); }
    });
    clearTextures();
    invalidateTrajectory();
}

void Viewport::invalidateTrajectory() {
    trajectoryCache.clear();
    trajectoryNode = -1;
}
float Viewport::sceneHeight() {
    if (cachedRestHeight < 0)
        cachedRestHeight = doc->scene.height();
    return cachedRestHeight;
}
QJsonObject Viewport::renderingStats() const { return renderer->diagnostics(); }
bool Viewport::isValid() const { return renderer->diagnostics()["pipelineHasView"].toBool(); }
QImage Viewport::grabFramebuffer() const {
    const_cast<Viewport*>(this)->syncRenderer();
    return renderer->captureImage();
}
QImage Viewport::captureOverlay() const {
    QImage image(size()*devicePixelRatioF(),QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatioF());
    image.fill(Qt::transparent);
    overlay->render(&image);
    return image;
}
void Viewport::syncRenderer() {
    matrices();
    paintedDocument=doc;
    paintedSession=doc->playbackSession;
    paintedOrdinal=doc->playbackOrdinal;
    renderer->setEditorCamera(view,projection);
    renderer->syncEditorScene(doc->scene,doc->meshRevision,doc->materialRevision,showMesh,diagnosticTransparent);
}
void Viewport::clearTextures() {
    cachedRestHeight=-1;
    if (renderer) renderer->invalidateEditorScene();
}
void Viewport::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    renderer->setGeometry(rect());
    static_cast<ViewOverlay*>(overlay)->syncGeometry();
    if(autoFramed) fit();
    matrices();
}
void Viewport::matrices() {
    view.setToIdentity();
    float y = qDegreesToRadians(yaw), p = qDegreesToRadians(pitch);
    QVector3D direction(std::sin(y) * std::cos(p), std::sin(p), std::cos(y) * std::cos(p));
    view.lookAt(center + direction * distance, center, {0, 1, 0});
    projection.setToIdentity();
    projection.perspective(40, float(width()) / std::max(1, height()), std::max(distance * .001f, .0001f),
                           distance * 100 + 100);
}
QPointF Viewport::project(QVector3D p) const {
    QVector4D clip = projection * view * QVector4D(p, 1);
    if (clip.w() <= 0)
        return {-10000, -10000};
    auto n = clip.toVector3DAffine();
    return {(n.x() + 1) * width() * .5, (1 - n.y()) * height() * .5};
}
QVector3D Viewport::cameraRight() const {
    return {view(0, 0), view(0, 1), view(0, 2)};
}
QVector3D Viewport::cameraUp() const {
    return {view(1, 0), view(1, 1), view(1, 2)};
}
float Viewport::scaleAt(QVector3D p) const {
    float z = -(view * QVector4D(p, 1)).z();
    return 2 * std::max(z, .001f) * std::tan(qDegreesToRadians(20.f)) / std::max(1, height());
}
QVector3D Viewport::axisVector(int i) const {
    QVector3D a(i == 0, i == 1, i == 2);
    if (local && doc->selected >= 0) {
        auto g = doc->scene.globals(doc->scene.pose);
        return g[doc->selected].mapVector(a).normalized();
    }
    return a;
}
void Viewport::fit() {
    autoFramed = true;
    auto p = doc->scene.positions(doc->scene.restPose());
    if (p.empty()) {
        center = {0, .8f, 0};
        distance = 4.2f;
        yaw = 24;
        pitch = 12;
        matrices();
        update();
        return;
    }
    auto [lo, hi] = doc->scene.bounds();
    if (physicsPreview && showPhysicsPreview) {
        physicsPreview->evaluate(float(doc->frame) * physicsPreview->fps / doc->scene.fps);
        auto previewBounds = physicsPreview->bounds();
        for (int j = 0; j < 3; ++j) {
            lo[j] = std::min(lo[j], previewBounds.first[j]);
            hi[j] = std::max(hi[j], previewBounds.second[j]);
        }
    }
    center = (lo + hi) * .5f;
    float aspect = float(width()) / std::max(1, height());
    matrices();
    float halfX = 0, halfY = 0, depth = 0;
    auto right = cameraRight(), up = cameraUp();
    QVector3D back{view(2, 0), view(2, 1), view(2, 2)};
    for (int mask = 0; mask < 8; mask++) {
        QVector3D v{mask & 1 ? hi.x() : lo.x(), mask & 2 ? hi.y() : lo.y(), mask & 4 ? hi.z() : lo.z()};
        v -= center;
        halfX = std::max(halfX, std::abs(QVector3D::dotProduct(v, right)));
        halfY = std::max(halfY, std::abs(QVector3D::dotProduct(v, up)));
        depth = std::max(depth, std::abs(QVector3D::dotProduct(v, back)));
    }
    distance = std::max(
        .2f,
        std::max(halfY, halfX / std::max(.1f, aspect)) / (std::tan(qDegreesToRadians(20.f)) * .88f) + depth);
    matrices();
    update();
}
void Viewport::viewPreset(int n) {
    yaw = n == 1 ? 0 : n == 2 ? 90 : 24;
    pitch = n == 3 ? 89 : n == 0 ? 12 : 0;
    matrices();
    update();
}
void Viewport::drawOverlay(QPainter &painter) {
    painter.setRenderHint(QPainter::Antialiasing);
    auto &s = doc->scene;
    auto positions = s.positions(s.pose);
    auto drawSkeleton = [&](const Pose &pose, QColor color, bool ghost) {
        auto ps = s.positions(pose);
        for (int i = 0; i < int(s.nodes.size()); ++i) {
            if (!s.nodes[i].joint)
                continue;
            int par = s.nodes[i].parent;
            if (par >= 0) {
                painter.setPen(QPen(ghost ? color : QColor(16, 25, 35), ghost ? 2 : 5));
                painter.drawLine(project(ps[par]), project(ps[i]));
                painter.setPen(QPen(ghost ? color : QColor(211, 223, 232), ghost ? 1 : 2));
                painter.drawLine(project(ps[par]), project(ps[i]));
            }
            if (!ghost) {
                bool sel = doc->selection().contains(i), eff = s.isEffector(i), contact = false;
                for (const auto &c : s.contacts)
                    if (c.node == i && doc->frame >= c.first && doc->frame <= c.last)
                        contact = true;
                painter.setPen(QPen(QColor(16, 25, 35), 2));
                painter.setBrush(sel                               ? QColor(255, 193, 79)
                                 : (s.pins.contains(i) || contact) ? QColor(78, 222, 164)
                                 : eff                             ? QColor(93, 194, 231)
                                                                   : QColor(220, 229, 238));
                float r = sel ? 7 : eff ? 5.5f : 3.5f;
                bool partner = s.mirrorPartner(doc->selected) == i;
                if (partner) {
                    painter.setPen(QPen(QColor(203, 150, 255), 2));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawEllipse(project(ps[i]), r + 4, r + 4);
                }
                bool pelvis = s.profile.roles.value("pelvis", -1) == i ||
                              s.nodes[i].name.contains("pelvis", Qt::CaseInsensitive) ||
                              s.nodes[i].name.contains("hips", Qt::CaseInsensitive);
                painter.setBrush(sel                  ? QColor(255, 193, 79)
                                 : s.pins.contains(i) ? QColor(78, 222, 164)
                                 : eff                ? QColor(93, 194, 231)
                                                      : QColor(220, 229, 238));
                painter.setPen(
                    QPen(i == hovered ? QColor(255, 255, 255) : QColor(16, 25, 35), i == hovered ? 3 : 2));
                auto pt = project(ps[i]);
                if (eff || pelvis) {
                    r = sel ? 9 : 7;
                    painter.drawRoundedRect(QRectF(pt.x() - r, pt.y() - r, 2 * r, 2 * r), eff ? 2 : 6,
                                            eff ? 2 : 6);
                } else
                    painter.drawEllipse(pt, r > 6 ? r : 2.5f, r > 6 ? r : 2.5f);
            }
        }
    };
    if (physicsPreview && showPhysicsPreview && physicsPreview->nodes.size() == s.nodes.size()) {
        physicsPreview->evaluate(float(doc->frame) * physicsPreview->fps / doc->scene.fps);
        drawSkeleton(physicsPreview->pose, QColor(99, 235, 174, 180), true);
    }
    const auto diagnostics = physicsPreview && showPhysicsPreview ? physicsDiagnostics : s.physicsResult;
    if ((physicsPreview && showPhysicsPreview) || showPhysicsModel) {
        auto trajectory = diagnostics["trajectory"].toArray();
        QPolygonF originalPath, resultPath, originalEffector, resultEffector;
        auto read = [](QJsonValue value) {
            auto a = value.toArray();
            return QVector3D(float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()));
        };
        for (auto point : trajectory) {
            auto o = point.toObject();
            originalPath << project(read(o["sourceComM"]) / s.physics.metersPerUnit);
            resultPath << project(read(o["resultComM"]) / s.physics.metersPerUnit);
        }
        auto selectedId = QString::number(doc->selected);
        for (auto point : trajectory) {
            auto o = point.toObject();
            auto a = o["sourceEffectorsM"].toObject(), b = o["resultEffectorsM"].toObject();
            if (a.contains(selectedId) && b.contains(selectedId)) {
                originalEffector << project(read(a[selectedId]) / s.physics.metersPerUnit);
                resultEffector << project(read(b[selectedId]) / s.physics.metersPerUnit);
            }
        }
        painter.setPen(QPen(QColor(238, 169, 83), 1, Qt::DotLine));
        painter.drawPolyline(originalEffector);
        painter.setPen(QPen(QColor(99, 235, 174), 1, Qt::DotLine));
        painter.drawPolyline(resultEffector);
        painter.setPen(QPen(QColor(238, 169, 83), 1.5, Qt::DashLine));
        painter.drawPolyline(originalPath);
        painter.setPen(QPen(QColor(99, 235, 174), 2));
        painter.drawPolyline(resultPath);
        for (auto item : diagnostics["contactForces"].toArray()) {
            auto force = item.toObject();
            if (std::abs(force["frame"].toDouble() -
                         double(doc->frame) * diagnostics["sourceFps"].toDouble(s.fps) / s.fps) > .3)
                continue;
            auto origin = read(force["positionM"]) / s.physics.metersPerUnit;
            auto world = read(force["forceWorldN"]);
            if (world.lengthSquared() < 1e-10)
                world = {0, float(force["normalForceN"].toDouble()), 0};
            auto end =
                origin + world * float(sceneHeight() * .35 /
                                       std::max(1.f, physicalMass(s) * std::max(1.f, s.physics.gravity)));
            painter.setPen(QPen(QColor(250, 146, 96), 2));
            painter.drawLine(project(origin), project(end));
            painter.setBrush(QColor(250, 146, 96));
            painter.drawEllipse(project(end), 3, 3);
        }
        if (!diagnostics.isEmpty()) {
            if (!trajectory.empty()) {
                painter.setPen(QColor(238, 169, 83));
                painter.drawText(16, 65, "COM: source / ");
                painter.setPen(QColor(99, 235, 174));
                painter.drawText(16 + painter.fontMetrics().horizontalAdvance("COM: source / "), 65,
                                 "result");
            }
            painter.setPen(diagnostics["status"] == "solved" ? QColor(99, 235, 174) : QColor(238, 169, 83));
            painter.drawText(16, trajectory.empty() ? 65 : 85,
                             diagnostics["status"].toString() +
                                 (diagnostics["staticHintOnly"].toBool() ? " (static)" : ""));
        }
    }
    if (showPhysicsModel) {
        auto globals = s.globals(s.pose);
        auto defs = s.bodies.empty() ? fittedPhysicsRig(s) : s.bodies;
        for (const auto &body : defs) {
            auto start = globals[body.node].map(QVector3D{}), end = globals[body.node].map(body.end),
                 com = globals[body.node].map(body.center);
            float radius = body.radius * globals[body.node].mapVector({1, 0, 0}).length();
            painter.setPen(QPen(QColor(128, 185, 234, 110), std::max(1.f, 2 * radius / scaleAt(start)),
                                Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(project(start), project(end));
            if (body.footHalfSize.lengthSquared() > 0) {
                QVector3D corners[8];
                for (int bits = 0; bits < 8; ++bits) {
                    QVector3D p;
                    for (int k = 0; k < 3; ++k)
                        p[k] = ((bits & (1 << k)) ? 1 : -1) * body.footHalfSize[k];
                    corners[bits] = globals[body.node].map(body.center + body.shapeRotation.rotatedVector(p));
                }
                painter.setPen(QPen(QColor(128, 185, 234), 1.5));
                for (int bits = 0; bits < 8; ++bits)
                    for (int k = 0; k < 3; ++k)
                        if (!(bits & (1 << k)))
                            painter.drawLine(project(corners[bits]), project(corners[bits | (1 << k)]));
            }
            painter.setPen(QPen(QColor(220, 205, 99), 1));
            painter.setBrush(QColor(220, 205, 99));
            painter.drawEllipse(project(com), 2.5, 2.5);
            if (body.node == doc->selected) {
                auto axis =
                    s.globalRotation(body.node, s.pose).rotatedVector(s.jointLimits.value(body.node).axis);
                painter.setPen(QPen(QColor(235, 110, 210), 2));
                painter.drawLine(project(start), project(start + axis * sceneHeight() * .12f));
            }
        }
    }
    if (showGhost && !s.keys.isEmpty()) {
        auto hi = s.keys.upperBound(doc->frame);
        if (hi != s.keys.end())
            drawSkeleton(hi->pose, {101, 159, 207, 75}, true);
        if (hi != s.keys.begin()) {
            --hi;
            if (hi.key() == doc->frame && hi != s.keys.begin())
                --hi;
            drawSkeleton(hi->pose, {225, 154, 93, 75}, true);
        }
    }
    if (showBones)
        drawSkeleton(s.pose, {}, false);
    if (showTrajectory && doc->selected >= 0 && !s.keys.isEmpty()) {
        if (trajectoryNode != doc->selected || trajectoryCache.empty()) {
            auto sample = s.motionOnlyCopy();
            trajectoryCache.clear();
            trajectoryNode = doc->selected;
            int step = std::max(1, (s.last - s.first) / 200);
            for (int f = s.first; f <= s.last; f += step) {
                sample.evaluate(float(f));
                trajectoryCache.push_back(sample.positions(sample.pose)[doc->selected]);
            }
        }
        QPolygonF path;
        for (auto point : trajectoryCache)
            path << project(point);
        painter.setPen(QPen(QColor(239, 184, 91, 160), 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(path);
    }
    if (showBalance) {
        auto b = s.balance(sceneHeight());
        QVector3D ground = b.center;
        ground.setY(s.physics.ground + .003f);
        QColor c = b.supported ? QColor(106, 221, 157) : QColor(248, 168, 102);
        QPolygonF support;
        for (auto v : b.support)
            support << project(v);
        painter.setPen(QPen(c, 1.5));
        painter.setBrush(QColor(c.red(), c.green(), c.blue(), 45));
        painter.drawPolygon(support);
        painter.setPen(QPen(c, 1, Qt::DashLine));
        painter.drawLine(project(b.center), project(ground));
        painter.setBrush(c);
        painter.drawEllipse(project(b.center), 5, 5);
        painter.drawEllipse(project(ground), 4, 4);
        painter.drawText(16, 46,
                         b.support.empty() ? "Static estimate · no foot contacts"
                         : b.supported     ? "Static estimate · COM over support"
                                           : "Static estimate · COM outside support");
        painter.setPen(QColor(170, 183, 198));
    }
    if (doc->selected >= 0) {
        int n = doc->selected;
        QVector3D origin = positions[n];
        QPointF o = project(origin);
        float len = scaleAt(origin) * std::min(75.f, height() * .22f);
        QColor colors[] = {QColor(241, 103, 105), QColor(115, 216, 142), QColor(105, 160, 247)};
        for (int a = 0; tool != Select && a < 3; ++a) {
            painter.setPen(QPen(colors[a], axis == a ? 4 : 2.5));
            if (tool == Move) {
                auto end = project(origin + axisVector(a) * len);
                painter.drawLine(o, end);
                painter.setBrush(colors[a]);
                painter.drawEllipse(end, 4, 4);
                painter.drawText(end + QPointF(6, -4), QString("XYZ")[a]);
            } else {
                QVector3D u = axisVector((a + 1) % 3), v = axisVector((a + 2) % 3);
                QPolygonF ring;
                for (int i = 0; i <= 64; ++i) {
                    float ang = float(i) / 64 * 2 * float(M_PI);
                    ring << project(origin + (u * std::cos(ang) + v * std::sin(ang)) * len * .75f);
                }
                painter.setBrush(Qt::NoBrush);
                painter.drawPolyline(ring);
            }
        }
        if (s.isEffector(n)) {
            QVector3D pole = s.pins.contains(n) ? s.pins[n].pole : s.defaultPole(n);
            painter.setPen(QPen(QColor(205, 154, 244), 1, Qt::DashLine));
            painter.drawLine(project(pole), o);
            painter.setBrush(QColor(205, 154, 244));
            auto p = project(pole);
            painter.drawPolygon(
                QPolygonF{p + QPointF(0, -7), p + QPointF(7, 0), p + QPointF(0, 7), p + QPointF(-7, 0)});
            painter.drawText(p + QPointF(10, 4), "Pole");
        }
        painter.setPen(QColor(255, 205, 107));
        painter.drawText(o + QPointF(10, -12), s.nodes[n].name);
    }
    if (!motionPaths.isEmpty()) {
        bool stale = motionPathRevision != doc->motionRevision;
        int legend = 42;
        for (auto it = motionPaths.begin(); it != motionPaths.end(); ++it) {
            painter.setPen(stale ? QColor(150, 150, 150) : QColor(220, 225, 235));
            painter.drawText(
                12, legend,
                QString("%1 / %2").arg(
                    it.key() < int(s.nodes.size()) ? s.nodes[it.key()].name : QString::number(it.key()),
                    stale ? "stale — update path" : "past red / future green / " + motionPathReferenceLabel));
            legend += 16;
            auto &points = it.value();
            double now = double(doc->frame) / s.fps;
            if (motionPathLabels && !stale) {
                int eventLabels = 0;
                for (auto annotation : s.annotations) {
                    auto event = annotation.toObject();
                    auto bound = event["nodes"].toArray();
                    if (!bound.empty() && !bound.contains(it.key()))
                        continue;
                    if (eventLabels++ >= 16)
                        break;
                    if (points.empty())
                        continue;
                    auto closest = std::lower_bound(points.begin(), points.end(), event["start"].toDouble(),
                                                    [](const auto &p, double t) { return p.first < t; });
                    if (closest == points.end())
                        continue;
                    auto p = project(closest->second);
                    painter.drawEllipse(p, 3, 3);
                    painter.drawText(p + QPointF(5, -5), event["name"].toString());
                }
            }
            QPointF lastFrameLabel(-1e9,-1e9);
            for (size_t i = 1; i < points.size(); ++i) {
                auto breaks = motionPathBreaks.constFind(it.key());
                bool discontinuous = false;
                if (breaks != motionPathBreaks.cend()) {
                    auto boundary = breaks.value().lower_bound(points[i - 1].first);
                    discontinuous = boundary != breaks.value().end() && *boundary <= points[i].first;
                }
                if (discontinuous ||
                    (motionPathPastSeconds >= 0 && points[i].first < now - motionPathPastSeconds) ||
                    (motionPathFutureSeconds >= 0 && points[i - 1].first > now + motionPathFutureSeconds))
                    continue;
                bool past = points[i].first < double(doc->frame) / s.fps;
                QPen pen(stale  ? QColor(150, 150, 150)
                         : past ? QColor(235, 85, 85)
                                : QColor(80, 220, 140),
                         2, stale || past ? Qt::DashLine : Qt::SolidLine);
                if (motionPathSpeedColors && !stale) {
                    double speed = motionPathSpeeds.value(it.key()).value(points[i].first, 0);
                    pen.setColor(QColor::fromHsvF(.65 * (1 - std::clamp(speed / 3., 0., 1.)), .8, .95));
                }
                painter.setPen(pen);
                painter.drawLine(project(points[i - 1].second), project(points[i].second));
                auto labelPoint=project(points[i].second);
                if (motionPathLabels && i % std::max(size_t(1), points.size() / 12) == 0 && QLineF(lastFrameLabel,labelPoint).length()>18) {
                    painter.drawText(labelPoint + QPointF(3, 9),QString::number(points[i].first * s.fps, 'f', 0));
                    lastFrameLabel=labelPoint;
                }
            }
        }
    }
    if (diagnosticOverlay && !selectedMotionProblem.isEmpty()) {
        bool stale = doc->analysisMotionRevision != doc->motionRevision ||
                     doc->analysisMetadataRevision != doc->metadataRevision;
        painter.setPen(QPen(stale ? QColor(150, 150, 150) : QColor(255, 90, 90), 3));
        auto positions = s.positions(s.pose);
        std::vector<QPointF> points;
        for (auto entry : selectedMotionProblem["nodes"].toArray()) {
            int n = entry.toInt(-1);
            if (n < 0 || n >= int(positions.size()))
                continue;
            auto point = project(positions[n]);
            points.push_back(point);
            painter.drawEllipse(point, 12, 12);
            painter.drawText(point + QPointF(12, 0), s.nodes[n].name);
        }
        for (auto entry : selectedMotionProblem["meshes"].toArray()) {
            int n = entry.toInt(-1);
            if (n < 0 || n >= int(s.meshes.size()))
                continue;
            auto vertices = s.skinned(s.meshes[n]);
            QRectF box;
            bool first = true;
            for (auto vertex : vertices) {
                auto point = project(vertex);
                if (first) {
                    box = QRectF(point, QSizeF(.01, .01));
                    first = false;
                } else
                    box = box.united(QRectF(point, QSizeF(.01, .01)));
            }
            painter.drawRect(box); // projected region, not a fabricated intersection point
        }
        if (points.size() == 2)
            painter.drawLine(points[0], points[1]);
        auto witness = selectedMotionProblem["pointMetres"].toArray();
        if (witness.size() == 3) {
            QVector3D world(float(witness[0].toDouble() / s.physics.metersPerUnit),
                            float(witness[1].toDouble() / s.physics.metersPerUnit),
                            float(witness[2].toDouble() / s.physics.metersPerUnit));
            auto point = project(world);
            painter.drawLine(point + QPointF(-8, 0), point + QPointF(8, 0));
            painter.drawLine(point + QPointF(0, -8), point + QPointF(0, 8));
            auto direction = selectedMotionProblem["approximateSeparationDirection"].toArray();
            if (direction.size() == 3) {
                QVector3D axis(float(direction[0].toDouble()), float(direction[1].toDouble()),
                               float(direction[2].toDouble()));
                painter.drawLine(point, project(world + axis * sceneHeight() * .12f));
            }
        }
        painter.drawText(12, height() - 48,
                         QString("%1 / %2 / %3")
                             .arg(selectedMotionProblem["kind"].toString(),
                                  selectedMotionProblem["status"].toString(),
                                  stale ? "stale" : "sampled geometric diagnostic"));
    }
    if (selectingNodes) {
        painter.setPen(QPen(QColor(80, 180, 240), 1));
        painter.setBrush(QColor(60, 150, 220, 35));
        painter.drawRect(selectionRect.normalized());
    }
    painter.setPen(QColor(178, 185, 195));
    painter.drawText(width() - 100, 24, readOnly ? "Preview" : local ? "Local" : "World");
    painter.setPen(QColor(220, 226, 234));
    QString context = physicsPreview && showPhysicsPreview ? "View: calculated result"
                      : s.compareOriginal                  ? "View: source animation"
                      : s.correction().present             ? "View: correction / " + s.correction().name
                      : s.sourceClip >= 0                  ? "View: source animation"
                                                           : "View: posed keys";
    painter.drawText(12, height() - 12,
                     QFontMetrics(painter.font()).elidedText(context, Qt::ElideRight, width() - 24));
    if (doc->selected >= 0) {
        QString info =
            (doc->selection().size() > 1 ? QString("%1 selected | Active: ").arg(doc->selection().size())
                                         : QString()) +
            s.nodes[doc->selected].name + " | " +
            (tool == Move && s.isEffector(doc->selected) ? "IK" : "FK") + " | " + (local ? "Local" : "World");
        if (doc->symmetryEditing)
            info += s.mirrorPartner(doc->selected) >= 0
                        ? " | Mirror: " + s.nodes[s.mirrorPartner(doc->selected)].name
                        : " | No mirror pair";
        painter.drawText(12, height() - 32,
                         QFontMetrics(painter.font()).elidedText(info, Qt::ElideRight, width() - 24));
    }
    emit annotationsPaint(&painter);
}
std::optional<QPointF> Viewport::projectVisible(QVector3D p) const {
    auto clip=projection*view*QVector4D(p,1);
    if(clip.w()<=0 || clip.z() < -clip.w() || clip.z() > clip.w())return std::nullopt;
    auto point=project(p);
    if(!std::isfinite(point.x()) || !std::isfinite(point.y()) || !rect().contains(point.toPoint()))return std::nullopt;
    return point;
}
void Viewport::contextMenuEvent(QContextMenuEvent *e) {
    if(readOnly || QLineF(pressMouse,lastMouse).length()>5 || dragging)return;
    auto candidates=pickCandidates(e->pos());
    if(!candidates.empty() && !doc->selection().contains(candidates.front()))doc->select(candidates.front());
    QMenu menu(this);auto add=menu.addAction("Add note");add->setShortcut(QKeySequence("Ctrl+Alt+N"));
    if(menu.exec(e->globalPos())==add)emit addNoteRequested();
}
std::vector<int> Viewport::pickCandidates(QPointF point) const {
    const auto &s = doc->scene;
    auto positions = s.positions(s.pose);
    std::vector<std::pair<float, int>> candidates;
    for (int n = 0; n < int(positions.size()); n++)
        if (s.nodes[n].joint) {
            float distance = float(QLineF(project(positions[n]), point).length());
            if (distance < 14)
                candidates.push_back({distance - (s.isEffector(n) ? 3.f : 0.f), n});
        }
    std::sort(candidates.begin(), candidates.end());
    std::vector<int> result;
    for (auto c : candidates)
        result.push_back(c.second);
    return result;
}
void Viewport::mousePressEvent(QMouseEvent *e) {
    setFocus();
    lastMouse = pressMouse = e->position();
    if (readOnly)
        return;
    if (e->button() != Qt::LeftButton || (e->modifiers() & Qt::AltModifier))
        return;
    if (doc->playing)
        doc->play(false);
    auto &s = doc->scene;
    auto p = s.positions(s.pose);
    if (e->modifiers() & Qt::ControlModifier) {
        auto candidates = pickCandidates(e->position());
        QMenu menu;
        struct PathHit {
            int node;
            double seconds, distance;
        };
        std::vector<PathHit> hits;
        if (motionPathRevision == doc->motionRevision)
            for (auto it = motionPaths.begin(); it != motionPaths.end(); ++it)
                for (auto point : it.value()) {
                    double distance = QLineF(project(point.second), e->position()).length();
                    if (distance < 8)
                        hits.push_back({it.key(), point.first, distance});
                }
        std::sort(hits.begin(), hits.end(), [&](auto a, auto b) {
            return std::abs(a.seconds - double(doc->frame) / s.fps) <
                   std::abs(b.seconds - double(doc->frame) / s.fps);
        });
        for (size_t i = 0; i < std::min(size_t(20), hits.size()); ++i) {
            auto hit = hits[i];
            auto action = menu.addAction(
                QString("Path %1 / %2 s")
                    .arg(hit.node < int(s.nodes.size()) ? s.nodes[hit.node].name : QString::number(hit.node))
                    .arg(hit.seconds, 0, 'g', 8));
            action->setData(QVariantMap{{"node", hit.node}, {"seconds", hit.seconds}});
        }
        if (hits.size() > 20)
            menu.addAction("More coincident times: inspect the numerical table")->setEnabled(false);
        if (!hits.empty() && !candidates.empty())
            menu.addSeparator();
        for (int n : candidates) {
            auto action =
                menu.addAction(s.nodes[n].name + (s.isEffector(n) ? " [IK controller]" : " [FK joint]"));
            action->setData(n);
        }
        if (!candidates.empty() || !hits.empty())
            if (auto chosen = menu.exec(e->globalPosition().toPoint())) {
                if (chosen->data().metaType().id() == QMetaType::QVariantMap) {
                    auto data = chosen->data().toMap();
                    doc->seek(std::clamp(qRound(data["seconds"].toDouble() * s.fps), s.first, s.last));
                    doc->select(data["node"].toInt());
                } else
                    doc->select(chosen->data().toInt());
            }
        return;
    }
    if (e->modifiers() & Qt::ShiftModifier) {
        auto candidates = pickCandidates(e->position());
        if (!candidates.empty())
            doc->select(candidates.front(), true);
        else {
            selectingNodes = true;
            selectionAdd = true;
            selectionRect = QRectF(pressMouse, pressMouse);
            update();
        }
        return;
    }
    poleDrag = false;
    axis = -1;
    if (tool != Select && doc->selected >= 0) {
        int n = doc->selected;
        QVector3D origin = p[n];
        float len = scaleAt(origin) * std::min(75.f, height() * .22f);
        QPointF o = project(origin);
        if (s.isEffector(n)) {
            auto pole = s.pins.contains(n) ? s.pins[n].pole : s.defaultPole(n);
            if (QLineF(project(pole), e->position()).length() < 10 &&
                QLineF(project(pole), e->position()).length() + 3 <
                    QLineF(project(origin), e->position()).length())
                poleDrag = true;
        }
        if (!poleDrag) {
            float best = 9;
            for (int a = 0; a < 3; ++a) {
                float d = 100;
                if (tool == Move)
                    d = segmentDistance(e->position(), o, project(origin + axisVector(a) * len));
                else {
                    auto u = axisVector((a + 1) % 3), v = axisVector((a + 2) % 3);
                    QPointF prev;
                    for (int i = 0; i <= 64; ++i) {
                        float ang = float(i) / 64 * 2 * float(M_PI);
                        auto pt = project(origin + (u * std::cos(ang) + v * std::sin(ang)) * len * .75f);
                        if (i)
                            d = std::min(d, segmentDistance(e->position(), prev, pt));
                        prev = pt;
                    }
                }
                if (d < best && QLineF(o, e->position()).length() > 11) {
                    best = d;
                    axis = a;
                }
            }
        }
    }
    if (axis < 0 && !poleDrag) {
        auto candidates = pickCandidates(e->position());
        int nearest = candidates.empty() ? -1 : candidates.front();
        if (nearest < 0) {
            if (tool == Select) {
                selectingNodes = true;
                selectionAdd = false;
                selectionRect = QRectF(pressMouse, pressMouse);
                update();
                return;
            }
            doc->select(-1);
            return;
        }
        if (!doc->selection().contains(nearest) || tool == Select)
            doc->select(nearest);
        else
            doc->setSelection(doc->selection(), nearest);
    }
    if (tool == Select)
        return;
    if (s.compareOriginal ||
        (s.correction().present && (!s.correction().enabled || !s.correction().editing))) {
        emit doc->feedback(
            "Recording is off. Disable Compare and enable editing on the active correction layer.");
        return;
    }
    if (doc->symmetryEditing && s.sideNamed(doc->selected) && s.mirrorPartner(doc->selected) < 0)
        emit doc->feedback("No mirror partner assigned: only this side can change. Review Character profile "
                           "> Mirror pairs.");
    if (tool == Move && s.isEffector(doc->selected) && !s.ikDiagnostic(doc->selected).isEmpty()) {
        emit doc->feedback(s.ikDiagnostic(doc->selected));
        return;
    }
    dragNodes = doc->selection();
    for (int n : dragNodes)
        if (tool == Move && s.isEffector(n) && !s.ikDiagnostic(n).isEmpty()) {
            emit doc->feedback(s.ikDiagnostic(n));
            return;
        }
    dragging = true;
    doc->begin();
    dragPose = s.pose;
    dragPins = s.pins;
    dragOrigin = p[doc->selected];
    dragPole = s.isEffector(doc->selected) ? (s.pins.contains(doc->selected) ? s.pins[doc->selected].pole
                                                                             : s.defaultPole(doc->selected))
                                           : QVector3D();
    setCursor(Qt::ClosedHandCursor);
}
void Viewport::mouseMoveEvent(QMouseEvent *e) {
    QPointF delta = e->position() - lastMouse;
    lastMouse = e->position();
    if (selectingNodes) {
        selectionRect = QRectF(pressMouse, e->position());
        update();
        return;
    }
    if (dragging) {
        auto &s = doc->scene;
        s.pose = dragPose;
        s.pins = dragPins;
        int n = doc->selected;
        QPointF total = e->position() - pressMouse;
        QVector3D movement =
            (cameraRight() * float(total.x()) - cameraUp() * float(total.y())) * scaleAt(dragOrigin);
        if (axis >= 0 && tool == Move) {
            auto a = axisVector(axis);
            auto projected = project(dragOrigin + a) - project(dragOrigin);
            double l = QPointF::dotProduct(projected, projected);
            movement = l > 1 ? a * float(QPointF::dotProduct(total, projected) / l) : QVector3D();
        }
        if (poleDrag) {
            auto target = s.pins.contains(n) ? s.pins[n].target : dragOrigin;
            s.pins[n] = {target, dragPole + movement, 1};
            s.solvePins();
        } else if (tool == Move) {
            auto positions = s.positions(dragPose);
            for (int bone : dragNodes) {
                bool inherited = false;
                for (int parent = s.nodes[bone].parent; parent >= 0; parent = s.nodes[parent].parent)
                    if (dragNodes.contains(parent)) {
                        inherited = true;
                        break;
                    }
                if (inherited)
                    continue;
                if (s.isEffector(bone)) {
                    auto pole = dragPins.contains(bone) ? dragPins[bone].pole : s.defaultPole(bone);
                    auto target = positions[bone] + movement;
                    if (s.pins.contains(bone))
                        s.pins[bone].target = target;
                    s.reach(bone, target, pole);
                } else
                    s.translateWorld(bone, movement);
            }
            s.solvePins();
        } else {
            float angle = float(total.x() - total.y()) * .6f;
            if (axis >= 0) {
                QVector3D u = axisVector((axis + 1) % 3), v = axisVector((axis + 2) % 3);
                float len = scaleAt(dragOrigin) * 75 * .75f, best = 1e20f, theta = 0;
                for (int i = 0; i < 128; ++i) {
                    float t = float(i) / 128 * 2 * float(M_PI);
                    auto pt = project(dragOrigin + (u * std::cos(t) + v * std::sin(t)) * len);
                    float d = float(QLineF(pt, pressMouse).length());
                    if (d < best) {
                        best = d;
                        theta = t;
                    }
                }
                auto tangent =
                    (project(dragOrigin + (u * std::cos(theta + .01f) + v * std::sin(theta + .01f)) * len) -
                     project(dragOrigin + (u * std::cos(theta - .01f) + v * std::sin(theta - .01f)) * len)) /
                    .02;
                double magnitude = QPointF::dotProduct(tangent, tangent);
                if (magnitude > 1)
                    angle = qRadiansToDegrees(float(QPointF::dotProduct(total, tangent) / magnitude));
            }
            QVector3D a = axis >= 0 ? QVector3D(axis == 0, axis == 1, axis == 2)
                                    : QVector3D(view(2, 0), view(2, 1), view(2, 2));
            for (int bone : dragNodes) {
                bool inherited = false;
                for (int parent = s.nodes[bone].parent; parent >= 0; parent = s.nodes[parent].parent)
                    if (dragNodes.contains(parent)) {
                        inherited = true;
                        break;
                    }
                if (!inherited)
                    s.rotate(bone, a, angle, axis < 0 || !local);
            }
        }
        if (doc->symmetryEditing)
            for (int bone : dragNodes)
                if (!dragNodes.contains(s.mirrorPartner(bone)))
                    s.mirrorChanged(bone, dragPose);
        emit doc->changed();
        return;
    }
    if (e->buttons() == Qt::NoButton) {
        auto candidates = pickCandidates(e->position());
        int next = candidates.empty() ? -1 : candidates.front();
        if (next != hovered) {
            hovered = next;
            update();
        }
        if (hovered >= 0)
            setToolTip(doc->scene.nodes[hovered].name +
                       (doc->scene.isEffector(hovered) ? " / Move: IK chain" : " / Move: FK joint") +
                       " / Ctrl+click: choose overlapping bone");
    }
    if (e->buttons() & Qt::MiddleButton ||
        ((e->buttons() & Qt::RightButton) && (e->modifiers() & Qt::ShiftModifier))) {
        autoFramed = false;
        center += (-cameraRight() * float(delta.x()) + cameraUp() * float(delta.y())) * scaleAt(center);
        update();
    } else if ((readOnly && (e->buttons() & Qt::LeftButton)) || e->buttons() & Qt::RightButton ||
               ((e->buttons() & Qt::LeftButton) && (e->modifiers() & Qt::AltModifier))) {
        autoFramed = false;
        yaw -= float(delta.x()) * .4f;
        pitch = std::clamp(pitch + float(delta.y()) * .4f, -89.f, 89.f);
        update();
    }
}
void Viewport::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && selectingNodes) {
        auto nodes = selectionAdd ? doc->selection() : QSet<int>{};
        auto positions = doc->scene.positions(doc->scene.pose);
        auto rect = QRectF(pressMouse, e->position()).normalized();
        for (int n = 0; n < int(positions.size()); n++)
            if (doc->scene.nodes[n].joint && rect.contains(project(positions[n])))
                nodes.insert(n);
        selectingNodes = false;
        doc->setSelection(nodes, doc->selected);
        update();
        return;
    }
    if (e->button() == Qt::LeftButton && dragging) {
        dragging = false;
        unsetCursor();
        if (QLineF(pressMouse, e->position()).length() > 2)
            doc->commit((poleDrag       ? "Move pole "
                         : tool == Move ? "Move "
                                        : "Rotate ") +
                        (dragNodes.size() > 1 && !poleDrag
                             ? QString("%1 selected controllers").arg(dragNodes.size())
                             : doc->scene.nodes[doc->selected].name));
        else
            doc->cancel();
        axis = -1;
        update();
    }
}
void Viewport::wheelEvent(QWheelEvent *e) {
    autoFramed = false;
    distance = std::clamp(distance * std::exp(-e->angleDelta().y() * .001f), sceneHeight() * .05f,
                          sceneHeight() * 100);
    update();
}
bool Viewport::event(QEvent *e) {
    if (dragging && e->type() == QEvent::ShortcutOverride) {
        e->accept();
        return true;
    }
    return QWidget::event(e);
}
void Viewport::focusOutEvent(QFocusEvent *e) {
    selectingNodes = false;
    if (dragging) {
        dragging = false;
        axis = -1;
        unsetCursor();
        doc->cancel();
    }
    QWidget::focusOutEvent(e);
}
void Viewport::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && selectingNodes) {
        selectingNodes = false;
        update();
        return;
    }
    if (e->key() == Qt::Key_Escape && dragging) {
        dragging = false;
        unsetCursor();
        doc->cancel();
        axis = -1;
        update();
        return;
    }
    if (dragging) {
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}
} // namespace trickster

QImage trickster::captureEditorWidget(QWidget* widget) {
    auto composeViewport=[](Viewport* view) {
        auto frame=view->grabFramebuffer();
        if(frame.isNull()) return frame;
        frame.setDevicePixelRatio(view->devicePixelRatioF());
        QPainter painter(&frame);
        painter.drawImage(QPoint(),view->captureOverlay());
        return frame;
    };
    if(auto* view=qobject_cast<Viewport*>(widget)) return composeViewport(view);
    auto image=widget->grab().toImage();
    QPainter painter(&image);
    for(auto* view:widget->findChildren<Viewport*>())
        if(view->isVisibleTo(widget)) painter.drawImage(view->mapTo(widget,QPoint()),composeViewport(view));
    return image;
}
