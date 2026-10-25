#pragma once
#include "physics.h"
namespace trickster {
void recordPhysicsProvenance(const Scene &source, int first, int last, PhysicsReport &report,
                             const QString &modelXml);
void recordControlReplay(PhysicsReport &report, const QString &modelXml, const std::vector<double> &q0,
                         const std::vector<double> &v0, const std::vector<std::vector<double>> &positions,
                         const std::vector<std::vector<double>> &controls);
} // namespace trickster
