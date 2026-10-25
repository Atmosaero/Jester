#pragma once
#include "core/scene/core.h"
#include "io/project/project_format.h"
namespace trickster {
QJsonObject sceneToJson(const Scene &scene);
bool sceneFromJson(const QJsonObject &object,Scene &scene,QString &error);
bool saveProject(const QString &path,const Scene &scene,QString &error);
bool loadProject(const QString &path,Scene &scene,QString &error);
} // namespace trickster
