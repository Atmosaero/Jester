#include "io/project/project_format.h"
#include <QFileInfo>
#include <QRegularExpression>
namespace trickster {
SceneFileKind classifySceneFile(const QString &path) {
    const QFileInfo file(path);
    const auto suffix=file.suffix().toLower();
    if(suffix=="jst" && !file.completeBaseName().isEmpty() &&
       !file.completeBaseName().endsWith(".jst",Qt::CaseInsensitive))return SceneFileKind::JesterProject;
    if(suffix=="fbx")return SceneFileKind::Fbx;
    if(suffix=="glb")return SceneFileKind::Glb;
    if(suffix=="gltf")return SceneFileKind::Gltf;
    return SceneFileKind::Unsupported;
}
bool isNativeProjectFile(const QString &path) {return classifySceneFile(path)==SceneFileKind::JesterProject;}
bool isSupportedSceneFile(const QString &path) {return classifySceneFile(path)!=SceneFileKind::Unsupported;}
QString normalizedProjectSavePath(const QString &path,QString &error) {
    if(path.isEmpty() || QFileInfo(path).fileName().trimmed().isEmpty()) {
        error="Project Jester needs a filename.";return {};
    }
    if(isNativeProjectFile(path)) {error.clear();return path;}
    if(QFileInfo(path).suffix().isEmpty()) {error.clear();return path+".jst";}
    error="Jester project paths must use one .jst extension.";return {};
}
QString suggestedProjectFileName(QString name) {
    name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"),"_");
    name=name.trimmed();
    while(name.endsWith(".jst",Qt::CaseInsensitive))name.chop(4);
    while(name.endsWith('.') || name.endsWith(' '))name.chop(1);
    if(name.isEmpty())name="Untitled";
    if(QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$",QRegularExpression::CaseInsensitiveOption).match(name).hasMatch())name.prepend('_');
    return name+".jst";
}
} // namespace trickster
