#include "analysis_views.h"
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
namespace trickster {
namespace {
QString numeric(QJsonValue v) {
    return v.isDouble() ? QString::number(v.toDouble(), 'g', 10) : QString("undefined");
}
} // namespace
void AnalysisTableModel::setReport(QJsonObject value) {
    beginResetModel();
    report = std::move(value);
    rows = report["samples"].toArray();
    mode = report.contains("surfaceEnabled") ? 2 : report.contains("alignment") ? 1 : 0;
    indices.clear();
    for (int row = 0; row < rows.size(); ++row) {
        auto entries = rows[row].toObject()[mode == 2 ? "issues" : "bones"].toArray();
        for (int item = 0; item < entries.size(); ++item)
            indices.push_back({row, item});
    }
    columns = mode == 2   ? QStringList{"Seconds", "Issue",      "Objects", "Status", "Error / depth / angle",
                                      "Method",  "Uncertainty"}
              : mode == 1 ? QStringList{"Left seconds",
                                        "Right seconds",
                                        "Role",
                                        "Left bone",
                                        "Right bone",
                                        "Position error",
                                        "Angle degrees",
                                        "Position units",
                                        "Velocity error",
                                        "Angular velocity error",
                                        "Swing error deg",
                                        "Twist error deg",
                                        "Anatomy status"}
                          : QStringList{"Seconds",
                                        "Bone",
                                        "Px",
                                        "Py",
                                        "Pz",
                                        "Vx",
                                        "Vy",
                                        "Vz",
                                        "Ax",
                                        "Ay",
                                        "Az",
                                        "Wx",
                                        "Wy",
                                        "Wz",
                                        "Speed",
                                        "Distance since previous",
                                        "Derivative status",
                                        "Linear units",
                                        "Contact error m",
                                        "IK target error m",
                                        "Contact angle deg"};
    endResetModel();
}
int AnalysisTableModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(indices.size());
}
int AnalysisTableModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(columns.size());
}
QVariant AnalysisTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole)
        return {};
    return orientation == Qt::Horizontal && section < columns.size() ? QVariant(columns[section])
                                                                     : QVariant(section + 1);
}
double AnalysisTableModel::seconds(int row) const {
    if (row < 0 || row >= indices.size())
        return 0;
    auto r = rows[indices[row].first].toObject();
    return r[mode == 1 ? "leftSeconds" : "seconds"].toDouble();
}
int AnalysisTableModel::node(int row) const {
    if (row < 0 || row >= indices.size())
        return -1;
    auto r = rows[indices[row].first].toObject();
    auto b = r[mode == 2 ? "issues" : "bones"].toArray()[indices[row].second].toObject();
    return mode == 2 ? (b["nodes"].toArray().isEmpty() ? -1 : b["nodes"].toArray()[0].toInt(-1))
                     : b[mode == 1 ? "left" : "node"].toInt(-1);
}
QVariant AnalysisTableModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= indices.size() ||
        (role != Qt::DisplayRole && role != Qt::ToolTipRole))
        return {};
    auto pair = indices[index.row()];
    auto row = rows[pair.first].toObject(),
         item = row[mode == 2 ? "issues" : "bones"].toArray()[pair.second].toObject();
    int c = index.column();
    if (role == Qt::ToolTipRole)
        return mode == 2 ? item["uncertainty"].toString() + " " +
                               item["measurement"].toObject()["reason"].toString()
                         : report["method"].toString() + " " + report["referenceMeaning"].toString();
    if (c == 0)
        return numeric(row[mode == 1 ? "leftSeconds" : "seconds"]);
    if (mode == 2) {
        if (c == 1)
            return item["kind"].toString();
        if (c == 2) {
            QStringList names;
            for (auto v : item["nodes"].toArray())
                names << QString("bone %1").arg(v.toInt());
            for (auto v : item["meshes"].toArray())
                names << QString("mesh %1").arg(v.toInt());
            return names.join(", ");
        }
        if (c == 3)
            return item["status"].toString();
        if (c == 4)
            return item.contains("measurement")
                       ? numeric(item["measurement"].toObject()["error"])
                       : numeric(item.contains("angleDegrees") ? item["angleDegrees"] : item["depthMetres"]);
        if (c == 5)
            return item["method"].toString();
        return item["uncertainty"].toString();
    }
    if (mode == 1) {
        if (c == 1)
            return numeric(row["rightSeconds"]);
        if (c == 2)
            return item["role"].toString();
        if (c == 3)
            return item["left"].toInt();
        if (c == 4)
            return item["right"].toInt();
        if (c == 5)
            return numeric(item["positionError"]);
        if (c == 6)
            return numeric(item["orientationErrorDegrees"]);
        if (c == 7)
            return report["positionUnits"].toString();
        if (c == 8)
            return numeric(item["linearVelocityError"]);
        if (c == 9)
            return numeric(item["angularVelocityError"]);
        if (c == 10)
            return numeric(item["swingErrorDegrees"]);
        if (c == 11)
            return numeric(item["twistErrorDegrees"]);
        return item["anatomyStatus"].toString();
    }
    if (c == 1)
        return item["node"].toInt();
    if (c >= 2 && c <= 13) {
        QStringList quantities{"position", "velocity", "acceleration", "angularVelocity"};
        auto values = item[quantities[(c - 2) / 3]].toArray();
        return values.size() == 3 ? numeric(values[(c - 2) % 3]) : QString("undefined");
    }
    if (c == 14)
        return numeric(item["speed"]);
    if (c == 15)
        return numeric(item["distanceFromPrevious"]);
    if (c == 16)
        return item["derivativeStatus"].toString();
    if (c == 17)
        return report["positionUnits"].toString();
    if (c == 18)
        return numeric(item["contactErrorMetres"]);
    if (c == 19)
        return numeric(item["ikTargetErrorMetres"]);
    return numeric(item["contactAngleDegrees"]);
}
ComputedMotionPlot::ComputedMotionPlot(QWidget *parent) : QWidget(parent) {
    setMinimumSize(240, 120);
    setToolTip("Read-only snapshot graph; click to seek. Edit authored curves in Graph Editor.");
}
void ComputedMotionPlot::setReport(QJsonObject value) {
    report = std::move(value);
    rebuild();
}
void ComputedMotionPlot::setMetric(QString name, int axis, int bone) {
    metric = std::move(name);
    component = axis;
    node = bone;
    rebuild();
}
void ComputedMotionPlot::rebuild() {
    samples.clear();
    bool compare = report.contains("alignment");
    for (auto value : report["samples"].toArray()) {
        auto row = value.toObject();
        double time = row[compare ? "leftSeconds" : "seconds"].toDouble();
        for (auto b : row["bones"].toArray()) {
            auto bone = b.toObject();
            if (bone[compare ? "left" : "node"].toInt() != node)
                continue;
            auto v = bone[metric];
            if (v.isArray())
                v = v.toArray()[component];
            bool valid = v.isDouble() && std::isfinite(v.toDouble());
            samples.push_back({time, valid ? v.toDouble() : 0,
                               !valid || bone["derivativeStatus"] == "undefined-at-discontinuity"});
        }
    }
    if (samples.empty()) {
        first = low = 0;
        last = high = 1;
        update();
        return;
    }
    first = samples.front().t;
    last = std::max(first + 1e-6, samples.back().t);
    low = INFINITY;
    high = -INFINITY;
    for (auto v : samples)
        if (!v.gap) {
            low = std::min(low, v.value);
            high = std::max(high, v.value);
        }
    if (!std::isfinite(low)) {
        low = 0;
        high = 1;
    }
    double margin = std::max(1e-6, (high - low) * .1);
    low -= margin;
    high += margin;
    update();
}
void ComputedMotionPlot::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 32, 38));
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(200, 215, 225));
    QString units =
        metric.contains("Degrees") ? "degrees"
        : metric.startsWith("angularVelocity")
            ? "rad/s"
            : report["positionUnits"].toString() +
                  (metric == "velocity" || metric == "speed" || metric == "linearVelocityError" ? "/s"
                   : metric == "acceleration"                                                   ? "/s^2"
                                                                                                : "");
    p.drawText(8, 17,
               QString("Computed snapshot: %1[%2] / %3 / seconds").arg(metric).arg(component).arg(units));
    p.drawText(8, height() - 5, QString::number(first, 'g', 6));
    p.drawText(width() - 70, height() - 5, QString::number(last, 'g', 6));
    auto point = [&](Sample s) {
        return QPointF(45 + (s.t - first) / (last - first) * std::max(1, width() - 60),
                       height() - 24 - (s.value - low) / (high - low) * std::max(1, height() - 50));
    };
    p.setPen(QPen(QColor(105, 205, 180), 1.5));
    size_t stride = std::max(size_t(1), samples.size() / 5000);
    bool gap = true;
    QPointF previous;
    for (size_t i = 0; i < samples.size(); ++i) {
        auto sample = samples[i];
        if (sample.gap) {
            gap = true;
            continue;
        }
        if (i % stride && i + 1 < samples.size())
            continue;
        auto q = point(sample);
        if (!gap)
            p.drawLine(previous, q);
        previous = q;
        gap = false;
    }
    p.setPen(QColor(160, 175, 190));
    p.drawText(2, 36, QString::number(high, 'g', 4));
    p.drawText(2, height() - 28, QString::number(low, 'g', 4));
}
void ComputedMotionPlot::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && seek)
        seek(first +
             std::clamp((e->position().x() - 45) / std::max(1, width() - 60), 0., 1.) * (last - first));
}
} // namespace trickster
