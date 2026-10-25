#pragma once
#include "mujoco_backend.h"
namespace trickster {
// Normalized quaternion Hermite uses glTF CUBICSPLINE component tangents, in units/second.
// Source helpers/morphs remain sampled from the original authoring scene.
QJsonObject bakePhysicsTrajectory(const Scene &source, mj::Backend &backend,
                                  const std::vector<std::vector<double>> &positions, int first, int last,
                                  Scene &baked, bool adaptive = true, PhysicsControl *control = nullptr,
                                  const std::vector<double> &initialVelocity = {});
QJsonObject physicsIntervalBoundaries(const Scene &source, const Scene &baked, mj::Backend &backend,
                                      int first, int last);
QJsonObject comparePhysicsTrajectory(const Scene &baked, mj::Backend &backend,
                                     const std::vector<std::vector<double>> &positions, int first, int last,
                                     PhysicsControl *control = nullptr);
} // namespace trickster
