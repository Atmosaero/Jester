#pragma once
#include "core/scene/document.h"
#include <QWidget>
#include <QRectF>
#include <QPointF>
#include <QSet>
#include <vector>
namespace trickster {
class CurveCanvas : public QWidget {

  public:
    Document *doc;
    QString id;
    int drag = -1, handle = 0;
    bool panning = false, boxing = false;
    QRectF box;
    QSet<int> selected;
    QSet<QString> visibleChannels;
    std::vector<CurveKey> clipboard;
    std::vector<CurveKey> dragStart;
    QPointF dragPress;
    QPointF panPoint;
    double first = 0, last = 3, low = -1, high = 1;
    explicit CurveCanvas(Document *d);
    ScalarCurve *curve();
    QPointF point(double t, double v) const;
    QPointF keyPoint(const ScalarCurve &curve, const CurveKey &key) const;
    double time(double x) const;
    double value(double y) const;
    void fit();
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

};
} // namespace trickster
