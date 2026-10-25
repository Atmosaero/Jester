#pragma once
#include "core/scene/core.h"
#include "core/scene/runtime_error.h"
#include <QSaveFile>
#include <QtEndian>
#include <cstring>
namespace trickster::project_detail {

inline QJsonArray vec(QVector3D v) {
    return {v.x(), v.y(), v.z()};
}
inline QVector3D v3(QJsonArray a, QVector3D def = {}) {
    return a.size() == 3 ? QVector3D(float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()))
                         : def;
}
inline QJsonObject tr(const Transform &t) {
    return {{"t", vec(t.t)}, {"r", QJsonArray{t.r.x(), t.r.y(), t.r.z(), t.r.scalar()}}, {"s", vec(t.s)}};
}
inline Transform readTr(QJsonObject o) {
    auto r = o["r"].toArray();
    return {v3(o["t"].toArray()),
            r.size() == 4 ? QQuaternion(float(r[3].toDouble()), float(r[0].toDouble()),
                                        float(r[1].toDouble()), float(r[2].toDouble()))
                          : QQuaternion(),
            v3(o["s"].toArray(), {1, 1, 1})};
}
inline QJsonArray poseJson(const Pose &p) {
    QJsonArray a;
    for (const auto &t : p)
        a.append(tr(t));
    return a;
}
inline Pose readPose(QJsonArray a) {
    Pose p;
    for (auto t : a)
        p.push_back(readTr(t.toObject()));
    return p;
}
inline QJsonArray pinJson(const QMap<int, Pin> &pins) {
    QJsonArray a;
    for (auto it = pins.cbegin(); it != pins.cend(); ++it)
        a.append(QJsonObject{{"node", it.key()},
                             {"target", vec(it->target)},
                             {"pole", vec(it->pole)},
                             {"weight", it->weight}});
    return a;
}
inline QMap<int, Pin> readPins(QJsonArray a) {
    QMap<int, Pin> p;
    for (auto v : a) {
        auto o = v.toObject();
        p[o["node"].toInt(-1)] = {v3(o["target"].toArray()), v3(o["pole"].toArray()),
                                  float(o["weight"].toDouble(1))};
    }
    return p;
}
inline QJsonArray floats(const std::vector<float> &v) {
    QJsonArray a;
    for (float f : v)
        a.append(f);
    return a;
}
inline std::vector<float> readFloats(QJsonArray a) {
    std::vector<float> v;
    for (auto f : a)
        v.push_back(float(f.toDouble()));
    return v;
}
inline QJsonArray morphJson(const MorphWeights &w) {
    QJsonArray a;
    for (auto it = w.cbegin(); it != w.cend(); ++it)
        a.append(QJsonObject{{"node", it.key()}, {"weights", floats(it.value())}});
    return a;
}
inline MorphWeights readMorphs(QJsonArray a) {
    MorphWeights w;
    for (auto v : a) {
        auto o = v.toObject();
        w[o["node"].toInt(-1)] = readFloats(o["weights"].toArray());
    }
    return w;
}
inline QJsonArray vectors(const std::vector<QVector3D> &v) {
    QJsonArray a;
    for (auto p : v)
        a.append(vec(p));
    return a;
}
inline std::vector<QVector3D> readVectors(QJsonArray a) {
    std::vector<QVector3D> v;
    for (auto p : a)
        v.push_back(v3(p.toArray()));
    return v;
}
inline bool writeFile(const QString &path, const QByteArray &data, QString &error) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        error = "Cannot save " + path + ": " + f.errorString();
        return false;
    }
    return true;
}
[[noreturn]] inline void bad(const QString &s) {
    throw trickster::RuntimeError(s.toStdString());
}
inline void need(bool ok, const QString &s) {
    if (!ok)
        bad(s);
}
inline quint32 u32(const QByteArray &b, qsizetype pos) {
    need(pos >= 0 && pos + 4 <= b.size(), "Truncated binary data.");
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + pos));
}
inline void appendU32(QByteArray &b, quint32 n) {
    quint32 le = qToLittleEndian(n);
    b.append(reinterpret_cast<const char *>(&le), 4);
}
inline void appendFloat(QByteArray &b, float n) {
    quint32 u;
    std::memcpy(&u, &n, 4);
    appendU32(b, u);
}

} // namespace trickster::project_detail
