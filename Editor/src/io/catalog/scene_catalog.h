#pragma once
#include <QString>
#include <vector>
namespace trickster {
struct SceneChoice {
    enum Kind { Empty, Mannequin, Demo, File } kind = Empty;
    QString name, category, path, description;
};
std::vector<SceneChoice> sceneChoices();
} // namespace trickster
