#pragma once
#include "mujoco_backend.h"
#include "physics.h"
#include <functional>
namespace trickster {
using ConfigurationSampler = std::function<void(double seconds, double *qpos)>;
QJsonObject sampledConfigurationInverse(mj::Backend &backend, const ConfigurationSampler &sample,
                                        double firstSeconds, double lastSeconds, int analysisFps,
                                        PhysicsControl *control = nullptr, bool timeSeries = false);
// Same compiled model, time grid and estimator must be used for a before/after pair.
QJsonObject sampledMotionInverse(mj::Backend &backend, const Scene &scene, double firstSeconds,
                                 double lastSeconds, int analysisFps, PhysicsControl *control = nullptr);
using ControlSampler = std::function<void(double seconds, double *controls)>;
QJsonObject motionImpulseBalance(mj::Backend &backend, const ConfigurationSampler &sample,
                                 const ControlSampler &controls, double firstSeconds, double lastSeconds,
                                 int analysisFps, double derivativeHalfWidth,
                                 PhysicsControl *control = nullptr);
bool inverseResidualPassed(const QJsonObject &metrics, double mass, double gravity, double height);
} // namespace trickster
