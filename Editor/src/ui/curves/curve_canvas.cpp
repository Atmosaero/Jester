#include "ui/widgets/localized_ui.h"
#include "ui/curves/curve_canvas.h"
#include "core/scene/edit_service.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace trickster {
CurveCanvas::CurveCanvas(Document *d) : doc(d) {
        setMinimumSize(260, 190);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        connect(d, &Document::changed, this, QOverload<>::of(&QWidget::update));
    }
ScalarCurve *CurveCanvas::curve() {
        for (auto &c : doc->scene.curves)
            if (c.id == id)
                return &c;
        return nullptr;
    }
QPointF CurveCanvas::point(double t, double v) const {
        return {40 + (t - first) / (last - first) * std::max(1, width() - 55),
                height() - 25 - (v - low) / (high - low) * std::max(1, height() - 40)};
    }
QPointF CurveCanvas::keyPoint(const ScalarCurve &curve, const CurveKey &key) const {
    return point(key.time,curve.path=="rotationQuaternion"?sampleCurve(curve,key.time):key.value);
}
double CurveCanvas::time(double x) const {
        return first + (x - 40) / std::max(1, width() - 55) * (last - first);
    }
double CurveCanvas::value(double y) const {
        return low + (height() - 25 - y) / std::max(1, height() - 40) * (high - low);
    }
void CurveCanvas::fit() {
        first = double(doc->scene.first) / doc->scene.fps;
        last = std::max(first + .01, double(doc->scene.last) / doc->scene.fps);
        low = 0;
        high = 1;
        if (auto c = curve()) {
            low = high = c->keys.front().value;
            for (int i = 0; i <= 200; ++i) {
                double v = sampleCurve(*c, first + (last - first) * i / 200);
                low = std::min(low, v);
                high = std::max(high, v);
            }
            for (const auto &other : doc->scene.curves)
                if (other.path == c->path && visibleChannels.contains(other.id))
                    for (int i = 0; i <= 200; ++i) {
                        double v = sampleCurve(other, first + (last - first) * i / 200);
                        low = std::min(low, v);
                        high = std::max(high, v);
                    }
            double margin = std::max(.1, (high - low) * .15);
            low -= margin;
            high += margin;
        }
        update();
    }
void CurveCanvas::paintEvent(QPaintEvent *)  {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Base));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(8, 16, uiText("Author local channel / seconds"));
        p.drawLine(point(first, 0), point(last, 0));
        auto c = curve();
        if (!c)
            return;
        int legend = 30, color = 0;
        for (const auto &other : doc->scene.curves)
            if (other.id != id && other.path == c->path && visibleChannels.contains(other.id)) {
                QPainterPath overlay;
                for (int i = 0; i <= width(); ++i) {
                    double t = first + (last - first) * i / std::max(1, width());
                    auto q = point(t, sampleCurve(other, t));
                    if (i)
                        overlay.lineTo(q);
                    else
                        overlay.moveTo(q);
                }
                auto tint = QColor::fromHsv((color++ * 71 + 30) % 360, 160, 210);
                p.setPen(QPen(tint, 1));
                p.drawPath(overlay);
                p.drawText(8, legend,
                           doc->scene.nodes[other.node].name + " / " + other.path + " " +
                               QString::number(other.component));
                legend += 15;
            }
        QPainterPath path;
        for (int i = 0; i <= width(); ++i) {
            double t = first + (last - first) * i / std::max(1, width());
            auto q = point(t, sampleCurve(*c, t));
            if (i)
                path.lineTo(q);
            else
                path.moveTo(q);
        }
        p.setPen(QPen(palette().color(QPalette::Link), 2));
        p.drawPath(path);
        int keyIndex = 0;
        for (auto &k : c->keys) {
            p.setPen(palette().color(QPalette::Mid));
            auto a = keyPoint(*c,k);
            if (c->path != "rotationQuaternion" && (k.handles == "Free" || k.handles == "Aligned")) {
                auto b = point(k.time + k.outTime, k.value + k.outValue),
                     d = point(k.time + k.inTime, k.value + k.inValue);
                p.drawLine(a, b);
                p.drawLine(a, d);
                p.drawEllipse(b, 3, 3);
                p.drawEllipse(d, 3, 3);
            }
            p.setBrush(selected.contains(keyIndex++) ? palette().color(QPalette::Text) : palette().color(QPalette::Highlight));
            p.drawEllipse(a, 4, 4);
        }
        if (boxing) {
            p.setPen(QColor(100, 180, 240));
            p.setBrush(QColor(100, 180, 240, 40));
            p.drawRect(box.normalized());
        }
        p.setPen(QColor(230, 100, 90));
        p.drawLine(point(double(doc->frame) / doc->scene.fps, low),
                   point(double(doc->frame) / doc->scene.fps, high));
    }
void CurveCanvas::mousePressEvent(QMouseEvent *e)  {
        if (e->button() == Qt::MiddleButton || e->button() == Qt::RightButton) {
            panning = true;
            panPoint = e->position();
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        auto c = curve();
        if (!c)
            return;
        for (int i = 0; i < int(c->keys.size()); ++i) {
            auto &k = c->keys[i];
            if (c->path != "rotationQuaternion" && (k.handles == "Free" || k.handles == "Aligned"))
                for (int side : {-1, 1})
                    if (QLineF(point(k.time + (side < 0 ? k.inTime : k.outTime),
                                     k.value + (side < 0 ? k.inValue : k.outValue)),
                               e->position())
                            .length() < 8) {
                        drag = i;
                        handle = side;
                        doc->begin();
                        return;
                    }
        }
        for (int i = 0; i < int(c->keys.size()); ++i)
            if (QLineF(keyPoint(*c,c->keys[i]),
                       e->position())
                    .length() < 9) {
                if (e->modifiers() & Qt::ControlModifier) {
                    if (selected.contains(i))
                        selected.remove(i);
                    else
                        selected.insert(i);
                    update();
                    return;
                }
                if (!selected.contains(i)) {
                    selected.clear();
                    selected.insert(i);
                }
                drag = i;
                handle = 0;
                dragStart = c->keys;
                dragPress = e->position();
                doc->begin();
                return;
            }
        if (e->modifiers() & Qt::ShiftModifier) {
            boxing = true;
            box = QRectF(e->position(), e->position());
            if (!(e->modifiers() & Qt::ControlModifier))
                selected.clear();
            return;
        }
        doc->seek(std::clamp(int(std::round(time(e->position().x()) * doc->scene.fps)), doc->scene.first,
                             doc->scene.last));
    }
void CurveCanvas::mouseMoveEvent(QMouseEvent *e)  {
        if (boxing) {
            box.setBottomRight(e->position());
            update();
            return;
        }
        if (panning) {
            double dt = time(panPoint.x()) - time(e->position().x()),
                   dv = value(panPoint.y()) - value(e->position().y());
            first += dt;
            last += dt;
            low += dv;
            high += dv;
            panPoint = e->position();
            update();
            return;
        }
        if (drag < 0)
            return;
        auto c = curve();
        if (!c)
            return;
        auto &k = c->keys[drag];
        if (handle) {
            double dt = time(e->position().x()) - k.time, dv = value(e->position().y()) - k.value;
            if (handle < 0) {
                double maxTime = drag ? k.time - c->keys[drag - 1].time : 1e8;
                k.inTime = std::clamp(dt, -maxTime, -std::min(1e-6, maxTime * .01));
                k.inValue = dv;
                if (k.handles == "Aligned")
                    k.outValue = k.outTime * k.inValue / k.inTime;
            } else {
                double maxTime = drag + 1 < int(c->keys.size()) ? c->keys[drag + 1].time - k.time : 1e8;
                k.outTime = std::clamp(dt, std::min(1e-6, maxTime * .01), maxTime);
                k.outValue = dv;
                if (k.handles == "Aligned")
                    k.inValue = k.inTime * k.outValue / k.outTime;
            }
        } else {
            double dt = time(e->position().x()) - time(dragPress.x()),
                   dv = value(e->position().y()) - value(dragPress.y());
            double lower = -1e12, upper = 1e12;
            for (int i : selected)
                if (i < int(dragStart.size())) {
                    if (i > 0 && !selected.contains(i - 1))
                        lower = std::max(lower, std::nextafter(dragStart[i - 1].time, dragStart[i].time) -
                                                    dragStart[i].time);
                    if (i + 1 < int(dragStart.size()) && !selected.contains(i + 1))
                        upper = std::min(upper, std::nextafter(dragStart[i + 1].time, dragStart[i].time) -
                                                    dragStart[i].time);
                }
            dt = std::clamp(dt, lower, upper);
            for (int i : selected)
                if (i < int(c->keys.size())) {
                    c->keys[i].time = dragStart[i].time + dt;
                    c->keys[i].value = dragStart[i].value + dv;
                }
        }
        doc->scene.evaluate(doc->frame);
        emit doc->changed();
    }
void CurveCanvas::mouseReleaseEvent(QMouseEvent *)  {
        if (boxing) {
            boxing = false;
            if (auto c = curve())
                for (int i = 0; i < int(c->keys.size()); ++i)
                    if (box.normalized().contains(keyPoint(*c,c->keys[i])))
                        selected.insert(i);
            double a = time(box.left()), b = time(box.right());
            doc->selectTime(a, b);
            update();
            return;
        }
        panning = false;
        if (drag < 0)
            return;
        drag = -1;
        QString error;
        if (validateCurves(doc->scene, &error))
            doc->commit(handle ? "Drag curve tangent" : "Drag curve key", false);
        else {
            doc->cancel();
            emit doc->feedback(error);
        }
    }
void CurveCanvas::keyPressEvent(QKeyEvent *e)  {
        auto ptr = curve();
        if (!ptr) {
            QWidget::keyPressEvent(e);
            return;
        }
        if (e->matches(QKeySequence::Copy)) {
            clipboard.clear();
            for (int i : selected)
                if (i < int(ptr->keys.size()))
                    clipboard.push_back(ptr->keys[i]);
            std::sort(clipboard.begin(), clipboard.end(), [](auto &a, auto &b) { return a.time < b.time; });
            return;
        }
        auto c = *ptr;
        bool changed = false;
        if (e->key() == Qt::Key_I) {
            double time = double(doc->frame) / doc->scene.fps;
            auto found = std::find_if(c.keys.begin(), c.keys.end(),
                                      [&](auto &key) { return std::abs(key.time - time) < 1e-10; });
            if (found == c.keys.end()) {
                CurveKey key;
                key.time = time;
                key.value = sampleCurve(c, time);
                if (c.path == "rotationQuaternion")
                    key.orientation = sampleOrientation(c, time);
                c.keys.push_back(key);
                std::sort(c.keys.begin(), c.keys.end(), [](auto &a, auto &b) { return a.time < b.time; });
                changed = true;
            }
        } else if (e->matches(QKeySequence::Paste) && !clipboard.empty()) {
            double dt = double(doc->frame) / doc->scene.fps - clipboard.front().time;
            for (auto key : clipboard) {
                key.time += dt;
                auto it = std::find_if(c.keys.begin(), c.keys.end(),
                                       [&](auto &k) { return std::abs(k.time - key.time) < 1e-10; });
                if (it == c.keys.end())
                    c.keys.push_back(key);
                else
                    *it = key;
            }
            std::sort(c.keys.begin(), c.keys.end(), [](auto &a, auto &b) { return a.time < b.time; });
            changed = true;
        } else if (e->key() == Qt::Key_Delete) {
            for (int i = int(c.keys.size()) - 1; i >= 0; --i)
                if (selected.contains(i))
                    c.keys.erase(c.keys.begin() + i);
            changed = true;
        } else if (e->key() == Qt::Key_A && (e->modifiers() & Qt::ControlModifier)) {
            for (int i = 0; i < int(c.keys.size()); ++i)
                selected.insert(i);
            update();
            return;
        }
        if (changed) {
            try {
                if (c.keys.empty())
                    EditService::execute(*doc, "animation_curves", {{"operation", "remove"}, {"id", c.id}},
                                         "Delete curve keys");
                else
                    EditService::execute(*doc, "animation_curves",
                                         {{"operation", "set"}, {"id", c.id}, {"curve", curvesJson({c})[0]}},
                                         "Edit curve keys");
                selected.clear();
                update();
            } catch (const std::exception &error) {
                emit doc->feedback(error.what());
            }
            return;
        }
        QWidget::keyPressEvent(e);
    }
void CurveCanvas::wheelEvent(QWheelEvent *e)  {
        double f = e->angleDelta().y() > 0 ? .8 : 1.25, mid = time(e->position().x());
        first = mid + (first - mid) * f;
        last = mid + (last - mid) * f;
        update();
    }
} // namespace trickster
