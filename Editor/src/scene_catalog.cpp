#include "scene_catalog.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
namespace trickster {
std::vector<SceneChoice> sceneChoices() {
    std::vector<SceneChoice> result{{SceneChoice::Empty,
                                     "Empty scene",
                                     "Templates",
                                     {},
                                     "Start with an empty workspace. Import a character when ready."},
                                    {SceneChoice::Mannequin,
                                     "Trickster mannequin",
                                     "Templates",
                                     {},
                                     "Original rigged humanoid. A clean starting pose for FK and IK."},
                                    {SceneChoice::Demo,
                                     "Animation demo",
                                     "Templates",
                                     {},
                                     "Four key poses with pinned feet and hand IK."}};
    for (QString kind : {"walk", "jump", "hand"}) {
        QString file = kind + "-correction.trickster";
        for (auto root :
             {QCoreApplication::applicationDirPath() + "/examples",
              QCoreApplication::applicationDirPath() + "/../examples", QDir::currentPath() + "/examples"}) {
            QString path = QDir(root).absoluteFilePath(file);
            if (QFileInfo::exists(path)) {
                result.push_back({SceneChoice::File, "MVP · " + kind, "Templates", path,
                                  "Synthetic editable demo: keyed layers, comparison, contacts and 48 FPS "
                                  "export. MIT licensed."});
                break;
            }
        }
    }
    for (QString name : {"annotated-jump", "curves-paths", "penetration-and-contact", "retarget-target", "retarget-result"})
        for (QString root : {QCoreApplication::applicationDirPath()+"/examples/motion-tools",
                             QCoreApplication::applicationDirPath()+"/../examples/motion-tools",
                             QDir::currentPath()+"/examples/motion-tools"}) {
            QString path=QDir(root).filePath(name+".trickster");
            if (!QFileInfo::exists(path)) continue;
            result.push_back({SceneChoice::File, name, "Motion tools", path,
                              "Original MIT synthetic motion-tools example. Context, curves, analysis or retargeting."});
            break;
        }
    QStringList roots{QCoreApplication::applicationDirPath() + "/library",
                      QCoreApplication::applicationDirPath() + "/../library",
                      QDir::currentPath() + "/library",
                      QCoreApplication::applicationDirPath() + "/../../library"};
    QSettings settings;
    const auto extra = settings.value("sceneLibraryFolder").toString();
    if (!extra.isEmpty())
        roots << extra;
    // Read every manifest before listing files: a resource-only subtree must
    // also stay hidden when encountered via another root, a folder or Recent.
    std::vector<SceneChoice> library;
    QStringList resourceRoots;
    QSet<QString> manifests;
    for (const auto &root : roots) {
        QDirIterator it(root, {"catalog.json"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString manifest = QFileInfo(it.next()).canonicalFilePath();
            if (manifests.contains(manifest))
                continue;
            manifests.insert(manifest);
            QFile f(manifest);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const auto catalog = QJsonDocument::fromJson(f.readAll()).object();
            if (catalog["resourceOnly"].toBool()) {
                resourceRoots << QFileInfo(manifest).absolutePath() + "/";
                continue;
            }
            for (auto item : catalog["entries"].toArray()) {
                auto entry = item.toObject();
                auto path = QFileInfo(QFileInfo(manifest).dir().filePath(entry["file"].toString()))
                                .canonicalFilePath();
                if (path.isEmpty())
                    continue;
                library.push_back(
                    {SceneChoice::File, entry["name"].toString(QFileInfo(path).completeBaseName()),
                     entry["category"].toString("Local library"), path, entry["description"].toString()});
            }
        }
    }
    QSet<QString> checkedResourceParents;
    auto resourceOnly = [&](const QString &path) {
        for (const auto &root : resourceRoots)
            if (path.startsWith(root,
#ifdef Q_OS_WIN
                                Qt::CaseInsensitive
#else
                                Qt::CaseSensitive
#endif
                                ))
                return true;
        // A recent file may belong to a library that is no longer mounted.
        QDir directory = QFileInfo(path).dir();
        do {
            const auto parent = directory.canonicalPath();
            if (checkedResourceParents.contains(parent))
                break;
            checkedResourceParents.insert(parent);
            QFile manifest(directory.filePath("catalog.json"));
            if (manifest.open(QIODevice::ReadOnly) &&
                QJsonDocument::fromJson(manifest.readAll()).object()["resourceOnly"].toBool()) {
                resourceRoots << parent + "/";
                return true;
            }
        } while (directory.cdUp());
        return false;
    };
    QSet<QString> seen;
    for (const auto &choice : result)
        if (!choice.path.isEmpty())
            seen.insert(QFileInfo(choice.path).canonicalFilePath());
    auto append = [&](SceneChoice choice) {
        choice.path = QFileInfo(choice.path).canonicalFilePath();
        if (choice.path.isEmpty() || resourceOnly(choice.path) || seen.contains(choice.path))
            return;
        const auto ext = QFileInfo(choice.path).suffix().toLower();
        if (ext != "glb" && ext != "gltf" && ext != "fbx" && ext != "trickster")
            return;
        seen.insert(choice.path);
        result.push_back(std::move(choice));
    };
    for (auto &choice : library)
        append(std::move(choice));
    if (!extra.isEmpty()) {
        QDirIterator it(extra, {"*.glb", "*.gltf", "*.fbx", "*.trickster"}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString path = it.next();
            append({SceneChoice::File, QFileInfo(path).completeBaseName(), "Local library", path,
                    "Local scene"});
        }
    }
    for (const auto &path : settings.value("recentScenes").toStringList())
        append({SceneChoice::File, QFileInfo(path).completeBaseName(), "Recent", path, path});
    return result;
}
} // namespace trickster
