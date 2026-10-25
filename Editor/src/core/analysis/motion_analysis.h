#pragma once
#include "core/scene/core.h"
#include <atomic>
namespace trickster {
struct AnalysisControl {
    std::atomic<bool> cancel{false};
    std::atomic<int> progress{0};
};
QJsonObject sampleMotionInstant(const Scene &, const QJsonArray &nodes, double seconds, double step,
                                QString space = "world", QString units = "scene");
QJsonObject motionTrajectory(const Scene &, const QJsonObject &, AnalysisControl * = nullptr);
QJsonArray measureExpectations(const Scene &, double seconds, double step, AnalysisControl * = nullptr);
QJsonArray validationSampleTimes(const Scene &, const QJsonObject &, AnalysisControl * = nullptr);
QJsonObject validateMotion(const Scene &, const QJsonObject &, AnalysisControl * = nullptr);
QJsonObject compareMotion(const Scene &, const Scene &, const QJsonObject &, AnalysisControl * = nullptr);
} // namespace trickster
