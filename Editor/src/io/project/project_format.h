#pragma once
#include <QString>
namespace trickster {
enum class SceneFileKind { Unsupported, JesterProject, Fbx, Glb, Gltf };
SceneFileKind classifySceneFile(const QString &path);
bool isNativeProjectFile(const QString &path);
bool isSupportedSceneFile(const QString &path);
// Add .jst only to a name without an extension. Reject unsupported or repeated
// extensions instead of silently producing file.glb.jst or file.jst.jst.
QString normalizedProjectSavePath(const QString &path, QString &error);
QString suggestedProjectFileName(QString sceneName);
} // namespace trickster
