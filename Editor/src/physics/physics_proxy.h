#pragma once
#include "core/scene/core.h"
namespace trickster {
// Creates an explicit approximate copy. Original rig, skins and channels are never changed.
bool preparePhysicsProxy(const Scene &source, Scene &proxy, QJsonObject &report, QString &error,
                         int outputFps = 0);
} // namespace trickster
