#include "io/project/project_io.h"
#include "io/project/recovery.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
namespace trickster {
RecoveryStore::RecoveryStore(Workspace &workspace, QString directory, QObject *parent)
    : QObject(parent), workspace(workspace) {
    folder = directory.isEmpty()
                 ? qEnvironmentVariable(
                       "TRICKSTER_RECOVERY_DIR",
                       QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery")
                 : directory;
    connect(&timer, &QTimer::timeout, this, [this] {
        QString error;
        if (!flush(error))
            emit failed(error);
    });
}
QStringList RecoveryStore::snapshots() const {
    QStringList files;
    for (const auto &name : QDir(folder).entryList({"*.recovery.jst"}, QDir::Files, QDir::Time))
        files << QDir(folder).absoluteFilePath(name);
    return files;
}
void RecoveryStore::discard(const QString &id) {
    QFile::remove(folder + "/" + id + ".recovery.jst");
    saved.remove(id);
    if (recoveredFiles.contains(id)) {
        QFile::remove(recoveredFiles.take(id));
    }
}
bool RecoveryStore::flush(QString &error) {
    if (!QDir().mkpath(folder)) {
        error = "Cannot create autosave directory: " + folder;
        return false;
    }
    for (const auto &doc : workspace.documents) {
        if (doc->inTransaction())
            continue;
        if (!doc->dirty()) {
            discard(doc->id);
            continue;
        }
        if (saved.contains(doc->id) && saved[doc->id] == doc->revision)
            continue;
        if (!doc->scene.validate(&error))
            return false;
        QJsonObject value=sceneToJson(doc->scene);
        value["recovery"]=QJsonObject{{"kind","JesterRecovery"},{"version",1},
            {"documentId",doc->id},{"revision",qint64(doc->revision)},
            {"originalPath",doc->path},{"selected",doc->selected}};
        QSaveFile file(folder + "/" + doc->id + ".recovery.jst");
        auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            error = "Autosave failed: " + file.errorString();
            return false;
        }
        saved[doc->id] = doc->revision;
    }
    return true;
}
bool RecoveryStore::recover(const QString &path, QString &error) {
    if(!isNativeProjectFile(path)) {error="Jester recovery snapshots use .jst.";return false;}
    // A recovered document has no original output path. Save As is always explicit.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 512 * 1024 * 1024) {
        error = "Cannot read recovery snapshot.";
        return false;
    }
    QJsonParseError parse;
    auto value = QJsonDocument::fromJson(file.readAll(), &parse).object();
    const auto metadata=value["recovery"].toObject();
    if (parse.error != QJsonParseError::NoError || metadata["kind"] != "JesterRecovery" ||
        metadata["version"].toInt() != 1) {
        error = "Invalid recovery snapshot.";
        return false;
    }
    Scene scene;
    if (!sceneFromJson(value, scene, error))
        return false;
    scene.name += " · recovered";
    workspace.append(std::move(scene), {}, false);
    auto &d = workspace.active();
    if (QFileInfo(path).absolutePath() == QFileInfo(folder).absoluteFilePath())
        recoveredFiles[d.id] = QFileInfo(path).absoluteFilePath();
    d.select(metadata["selected"].toInt(-1));
    d.undo.resetClean();
    // Keep the snapshot until explicit save or discard; the source file is never overwritten.
    return true;
}
} // namespace trickster
