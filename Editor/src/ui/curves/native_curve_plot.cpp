#include "ui/curves/native_curve_plot.h"
#include "core/scene/edit_service.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace trickster {
NativeCurvePlot::NativeCurvePlot(const Scene &s) : snapshot(s.motionOnlyCopy()) {
        setMinimumHeight(180);
    }
double NativeCurvePlot::sample(double time) {
        const auto &c = (*snapshot.clips)[snapshot.sourceClip].channels[channel];
        snapshot.evaluateNativeSource(float(time * snapshot.fps));
        if (c.path == "translation")
            return double(snapshot.pose[c.node].t[component]);
        if (c.path == "scale")
            return double(snapshot.pose[c.node].s[component]);
        if (c.path == "weights")
            return double(snapshot.morphWeights(c.node)[component]);
        auto q = snapshot.pose[c.node].r;
        return double(component == 3 ? q.scalar() : q.vector()[component]);
    }
void NativeCurvePlot::paintEvent(QPaintEvent *)  {
        QPainter p(this);
        p.fillRect(rect(), QColor(28, 31, 36));
        p.setRenderHint(QPainter::Antialiasing);
        const auto &c = (*snapshot.clips)[snapshot.sourceClip].channels[channel];
        double a = c.times.front(), b = std::max(a + .001, double(c.times.back())), low = INFINITY,
               high = -INFINITY;
        std::vector<double> values;
        for (int i = 0; i <= 500; ++i) {
            double v = sample(a + (b - a) * i / 500);
            values.push_back(v);
            low = std::min(low, v);
            high = std::max(high, v);
        }
        high = std::max(high, low + .001);
        QPainterPath path;
        for (int i = 0; i <= 500; ++i) {
            QPointF point(25. + (width() - 50.) * i / 500,
                          height() - 25. - (height() - 50.) * (values[i] - low) / (high - low));
            if (i)
                path.lineTo(point);
            else
                path.moveTo(point);
        }
        p.setPen(QPen(QColor(130, 190, 240), 2));
        p.drawPath(path);
        p.drawText(8, 15, "Native source (read-only) / " + c.interpolation + " / seconds");
    }
} // namespace trickster
