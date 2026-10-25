#pragma once
#include "core/scene/document.h"
#include <QWidget>
#include <QRectF>
#include <QSet>
#include <vector>
namespace trickster {
class NativeCurvePlot : public QWidget {

  public:
    Scene snapshot;
    int channel = 0, component = 0;
    NativeCurvePlot(const Scene &s);
    double sample(double time);
    void paintEvent(QPaintEvent *) override;

};
} // namespace trickster
