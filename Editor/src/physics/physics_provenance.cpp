#include "io/project/project_io.h"
#include "physics/physics_provenance.h"
#include "core/scene/core.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
namespace trickster {
void recordPhysicsProvenance(const Scene &source, int first, int last, PhysicsReport &report,
                             const QString &modelXml) {
    auto proxy=source.physics.requests["rigProxy"].toObject();
    if(!proxy.isEmpty()) {
        report.details["rigProxy"]=proxy;
        report.warnings<<QString("Approximate input proxy: maximum measured geometry mapping error %1 mm.")
            .arg(proxy["maximumWorldGeometryErrorM"].toDouble()*1000,0,'g',5);
        if(report.status=="solved")report.status="approximate";
    }
    auto json = sceneToJson(source);
    json.remove("visuals"); // Rendering preferences are not physics inputs.
    report.details["inputHashScope"]="authored scene data excluding visual settings";
    report.details["inputSha256"] =
        QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(json).toJson(QJsonDocument::Compact),
                                                     QCryptographicHash::Sha256)
                                .toHex());
    if (!modelXml.isEmpty())
        report.details["modelSha256"] = QString::fromLatin1(
            QCryptographicHash::hash(modelXml.toUtf8(), QCryptographicHash::Sha256).toHex());
    report.details["modelSettings"] = json["physics"];
    QJsonArray contacts;
    for (const auto &c : physicsContactSchedule(source))
        contacts.append(QJsonObject{{"node", c.node},
                                    {"first", c.first},
                                    {"last", c.last},
                                    {"fadeIn", c.fadeIn},
                                    {"fadeOut", c.fadeOut},
                                    {"target", QJsonArray{c.target.x(), c.target.y(), c.target.z()}},
                                    {"pole", QJsonArray{c.pole.x(), c.pole.y(), c.pole.z()}},
                                    {"orient", c.orient},
                                    {"orientation", QJsonArray{c.orientation.x(), c.orientation.y(),
                                                               c.orientation.z(), c.orientation.scalar()}}});
    report.details["contactSchedule"] = contacts;
    report.details["sourceFps"] = source.fps;
    report.details["first"] = first;
    report.details["last"] = last;
    report.details["durationSeconds"] = double(last - first) / source.fps;
    double h = source.height() * source.physics.metersPerUnit, m = physicalMass(source),
           g = std::max(1.f, source.physics.gravity);
    report.details["normalization"] = QJsonObject{{"heightM", h},
                                                  {"massKg", m},
                                                  {"forceN", m * g},
                                                  {"torqueNm", m * g * h},
                                                  {"contactToleranceM", .01 * h},
                                                  {"penetrationToleranceM", .005 * h},
                                                  {"importantPoseToleranceM", .02 * h},
                                                  {"rootForceRmsToleranceN", .03 * m * g}};
}
void recordControlReplay(PhysicsReport &report, const QString &modelXml, const std::vector<double> &q0,
                         const std::vector<double> &v0, const std::vector<std::vector<double>> &positions,
                         const std::vector<std::vector<double>> &controls) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << quint32(0x54524b31) << quint32(q0.size()) << quint32(v0.size())
           << quint32(controls.front().size()) << quint32(controls.size());
    for (double x : q0)
        stream << x;
    for (double x : v0)
        stream << x;
    for (const auto &row : positions)
        for (double x : row)
            stream << x;
    for (const auto &row : controls)
        for (double x : row)
            stream << x;
    report.details["controlReplay"] = QJsonObject{
        {"format", "Trickster control replay 1: qCompress/base64, little-endian f64"},
        {"modelXml", modelXml},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        {"data", QString::fromLatin1(qCompress(bytes, 6).toBase64())}};
}
} // namespace trickster
