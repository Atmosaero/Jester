#include "core/scene/runtime_error.h"
#include "core/scene/core.h"
#include "trickster_version.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace trickster {
namespace {
QJsonArray vec(QVector3D v) {
    return {v.x(), v.y(), v.z()};
}
QVector3D v3(QJsonArray a, QVector3D def = {}) {
    return a.size() == 3 ? QVector3D(float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()))
                         : def;
}
QJsonObject tr(const Transform &t) {
    return {{"t", vec(t.t)}, {"r", QJsonArray{t.r.x(), t.r.y(), t.r.z(), t.r.scalar()}}, {"s", vec(t.s)}};
}
Transform readTr(QJsonObject o) {
    auto r = o["r"].toArray();
    return {v3(o["t"].toArray()),
            r.size() == 4 ? QQuaternion(float(r[3].toDouble()), float(r[0].toDouble()),
                                        float(r[1].toDouble()), float(r[2].toDouble()))
                          : QQuaternion(),
            v3(o["s"].toArray(), {1, 1, 1})};
}
QJsonArray poseJson(const Pose &p) {
    QJsonArray a;
    for (const auto &t : p)
        a.append(tr(t));
    return a;
}
Pose readPose(QJsonArray a) {
    Pose p;
    for (auto t : a)
        p.push_back(readTr(t.toObject()));
    return p;
}
QJsonArray pinJson(const QMap<int, Pin> &pins) {
    QJsonArray a;
    for (auto it = pins.cbegin(); it != pins.cend(); ++it)
        a.append(QJsonObject{{"node", it.key()},
                             {"target", vec(it->target)},
                             {"pole", vec(it->pole)},
                             {"weight", it->weight}});
    return a;
}
QMap<int, Pin> readPins(QJsonArray a) {
    QMap<int, Pin> p;
    for (auto v : a) {
        auto o = v.toObject();
        p[o["node"].toInt(-1)] = {v3(o["target"].toArray()), v3(o["pole"].toArray()),
                                  float(o["weight"].toDouble(1))};
    }
    return p;
}
QJsonArray floats(const std::vector<float> &v) {
    QJsonArray a;
    for (float f : v)
        a.append(f);
    return a;
}
std::vector<float> readFloats(QJsonArray a) {
    std::vector<float> v;
    for (auto f : a)
        v.push_back(float(f.toDouble()));
    return v;
}
QJsonArray morphJson(const MorphWeights &w) {
    QJsonArray a;
    for (auto it = w.cbegin(); it != w.cend(); ++it)
        a.append(QJsonObject{{"node", it.key()}, {"weights", floats(it.value())}});
    return a;
}
MorphWeights readMorphs(QJsonArray a) {
    MorphWeights w;
    for (auto v : a) {
        auto o = v.toObject();
        w[o["node"].toInt(-1)] = readFloats(o["weights"].toArray());
    }
    return w;
}
QJsonArray vectors(const std::vector<QVector3D> &v) {
    QJsonArray a;
    for (auto p : v)
        a.append(vec(p));
    return a;
}
std::vector<QVector3D> readVectors(QJsonArray a) {
    std::vector<QVector3D> v;
    for (auto p : a)
        v.push_back(v3(p.toArray()));
    return v;
}
bool writeFile(const QString &path, const QByteArray &data, QString &error) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        error = "Cannot save " + path + ": " + f.errorString();
        return false;
    }
    return true;
}
[[noreturn]] void bad(const QString &s) {
    throw trickster::RuntimeError(s.toStdString());
}
void need(bool ok, const QString &s) {
    if (!ok)
        bad(s);
}
quint32 u32(const QByteArray &b, qsizetype pos) {
    need(pos >= 0 && pos + 4 <= b.size(), "Truncated binary data.");
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + pos));
}
void appendU32(QByteArray &b, quint32 n) {
    quint32 le = qToLittleEndian(n);
    b.append(reinterpret_cast<const char *>(&le), 4);
}
void appendFloat(QByteArray &b, float n) {
    quint32 u;
    std::memcpy(&u, &n, 4);
    appendU32(b, u);
}
struct Access {
    int count = 0, components = 0;
    std::vector<float> data;
    float at(int i, int j = 0) const {
        need(i >= 0 && i < count && j >= 0 && j < components, "Accessor index out of range.");
        return data[size_t(i) * components + j];
    }
};
struct Reader {
    QJsonObject root;
    std::vector<QByteArray> buffers;
    Access accessor(int index) const {
        auto as = root["accessors"].toArray();
        need(index >= 0 && index < as.size(), "Missing accessor.");
        auto a = as[index].toObject();
        QString ty = a["type"].toString();
        int n = ty == "SCALAR" ? 1
                : ty == "VEC2" ? 2
                : ty == "VEC3" ? 3
                : ty == "VEC4" ? 4
                : ty == "MAT4" ? 16
                               : 0;
        need(n > 0, "Unsupported accessor shape.");
        int count = a["count"].toInt(-1), component = a["componentType"].toInt();
        int bytes = component == 5120 || component == 5121   ? 1
                    : component == 5122 || component == 5123 ? 2
                    : component == 5125 || component == 5126 ? 4
                                                             : 0;
        need(bytes > 0 && count >= 0 && count <= 2000000, "Unsupported or oversized accessor.");
        bool norm = a["normalized"].toBool();
        Access out{count, n, std::vector<float>(size_t(count) * n)};
        auto read = [&](int viewIndex, qint64 offset, int stride, int len, int comps, int ct, bool normalized,
                        std::vector<float> &dest) {
            auto views = root["bufferViews"].toArray();
            need(viewIndex >= 0 && viewIndex < views.size(), "Missing buffer view.");
            auto view = views[viewIndex].toObject();
            int buf = view["buffer"].toInt(-1);
            need(buf >= 0 && buf < int(buffers.size()), "Missing buffer.");
            int sz = ct == 5120 || ct == 5121 ? 1 : ct == 5122 || ct == 5123 ? 2 : 4;
            if (!stride)
                stride = view["byteStride"].toInt(comps * sz);
            need(stride >= comps * sz, "Invalid accessor stride.");
            qint64 base = view["byteOffset"].toInteger(), length = view["byteLength"].toInteger(-1);
            qint64 total = len ? qint64(len - 1) * stride + comps * sz : 0;
            need(offset >= 0 && base >= 0 && length >= 0 && offset + total <= length &&
                     base + length <= buffers[buf].size(),
                 "Accessor exceeds its buffer view.");
            const uchar *ptr = reinterpret_cast<const uchar *>(buffers[buf].constData() + base + offset);
            for (int i = 0; i < len; ++i)
                for (int j = 0; j < comps; ++j) {
                    const uchar *p = ptr + qint64(i) * stride + j * sz;
                    float v = 0;
                    if (ct == 5126) {
                        quint32 u = qFromLittleEndian<quint32>(p);
                        std::memcpy(&v, &u, 4);
                    } else if (ct == 5125)
                        v = float(qFromLittleEndian<quint32>(p));
                    else if (ct == 5123)
                        v = float(qFromLittleEndian<quint16>(p));
                    else if (ct == 5122)
                        v = float(qFromLittleEndian<qint16>(p));
                    else if (ct == 5121)
                        v = *p;
                    else
                        v = float(*reinterpret_cast<const qint8 *>(p));
                    if (normalized) {
                        if (ct == 5121)
                            v /= 255;
                        else if (ct == 5123)
                            v /= 65535;
                        else if (ct == 5120)
                            v = std::max(v / 127.f, -1.f);
                        else if (ct == 5122)
                            v = std::max(v / 32767.f, -1.f);
                    }
                    need(std::isfinite(v), "Nonfinite accessor value.");
                    dest[size_t(i) * comps + j] = v;
                }
        };
        if (a.contains("bufferView"))
            read(a["bufferView"].toInt(-1), a["byteOffset"].toInteger(), 0, count, n, component, norm,
                 out.data);
        if (a.contains("sparse")) {
            auto sp = a["sparse"].toObject(), indices = sp["indices"].toObject(),
                 values = sp["values"].toObject();
            int c = sp["count"].toInt(-1);
            need(c >= 0 && c <= count, "Invalid sparse accessor.");
            int ict = indices["componentType"].toInt();
            need(ict == 5121 || ict == 5123 || ict == 5125, "Invalid sparse index type.");
            std::vector<float> ix(c), val(size_t(c) * n);
            read(indices["bufferView"].toInt(-1), indices["byteOffset"].toInteger(),
                 ict == 5121   ? 1
                 : ict == 5123 ? 2
                               : 4,
                 c, 1, ict, false, ix);
            read(values["bufferView"].toInt(-1), values["byteOffset"].toInteger(), n * bytes, c, n, component,
                 norm, val);
            for (int i = 0; i < c; ++i) {
                int idx = int(ix[i]);
                need(idx >= 0 && idx < count, "Sparse index out of range.");
                for (int j = 0; j < n; ++j)
                    out.data[size_t(idx) * n + j] = val[size_t(i) * n + j];
            }
        }
        return out;
    }
};
Transform nodeTransform(QJsonObject o) {
    if (!o.contains("matrix")) {
        auto q = o["rotation"].toArray();
        return {v3(o["translation"].toArray()),
                q.size() == 4 ? QQuaternion(float(q[3].toDouble()), float(q[0].toDouble()),
                                            float(q[1].toDouble()), float(q[2].toDouble()))
                              : QQuaternion(),
                v3(o["scale"].toArray(), {1, 1, 1})};
    }
    auto a = o["matrix"].toArray();
    need(a.size() == 16, "Node matrix must have 16 elements.");
    QMatrix4x4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m(r, c) = float(a[c * 4 + r].toDouble());
    Transform t;
    t.t = m.column(3).toVector3D();
    QVector3D x = m.column(0).toVector3D(), y = m.column(1).toVector3D(), z = m.column(2).toVector3D();
    t.s = {x.length(), y.length(), z.length()};
    need(t.s.x() * t.s.y() * t.s.z() > 1e-12f, "Singular matrix.");
    if (m.determinant() < 0)
        t.s.setX(-t.s.x());
    x /= t.s.x();
    y /= t.s.y();
    z /= t.s.z();
    need(std::abs(QVector3D::dotProduct(x, y)) < .001f && std::abs(QVector3D::dotProduct(y, z)) < .001f &&
             std::abs(QVector3D::dotProduct(x, z)) < .001f,
         "Sheared node matrices are not supported.");
    QMatrix3x3 rot;
    for (int r = 0; r < 3; ++r) {
        rot(r, 0) = x[r];
        rot(r, 1) = y[r];
        rot(r, 2) = z[r];
    }
    t.r = QQuaternion::fromRotationMatrix(rot).normalized();
    return t;
}
} // namespace
QJsonObject sceneToJson(const Scene &s) {
    QJsonArray nodes, meshes, keys;
    for (const auto &n : s.nodes)
        nodes.append(QJsonObject{{"name", n.name},
                                 {"parent", n.parent},
                                 {"rest", tr(n.rest)},
                                 {"joint", n.joint},
                                 {"morphRest", floats(n.morphRest)}});
    for (const auto &m : s.meshes) {
        QJsonArray vertices, indices, joints, ib;
        for (const auto &v : m.vertices) {
            QJsonArray a{v.p.x(), v.p.y(), v.p.z()};
            for (int j : v.joints)
                a.append(j);
            for (float w : v.weights)
                a.append(w);
            a.append(v.normal.x());
            a.append(v.normal.y());
            a.append(v.normal.z());
            a.append(v.uv.x());
            a.append(v.uv.y());
            a.append(v.uv1.x());
            a.append(v.uv1.y());
            a.append(v.tangent.x());
            a.append(v.tangent.y());
            a.append(v.tangent.z());
            a.append(v.tangent.w());
            vertices.append(a);
        }
        QJsonArray targets;
        QJsonObject textures;
        for (const auto &t : m.targets)
            targets.append(QJsonObject{{"name", t.name},
                                       {"positions", vectors(t.positions)},
                                       {"normals", vectors(t.normals)},
                                       {"tangents", vectors(t.tangents)}});
        for (auto it = m.textures.cbegin(); it != m.textures.cend(); ++it) {
            const auto &t = it.value();
            textures[it.key()] = QJsonObject{{"bytes", QString::fromLatin1(t.bytes.toBase64())},
                                             {"mime", t.mime},
                                             {"coord", t.coord},
                                             {"wrapS", t.wrapS},
                                             {"wrapT", t.wrapT},
                                             {"minFilter", t.minFilter},
                                             {"magFilter", t.magFilter},
                                             {"offset", QJsonArray{t.offset.x(), t.offset.y()}},
                                             {"scale", QJsonArray{t.scale.x(), t.scale.y()}},
                                             {"rotation", t.rotation}};
        }
        for (auto i : m.indices)
            indices.append(int(i));
        for (int j : m.joints)
            joints.append(j);
        for (const auto &mat : m.inverseBind) {
            QJsonArray a;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    a.append(mat(r, c));
            ib.append(a);
        }
        meshes.append(QJsonObject{{"node", m.node},
                                  {"vertices", vertices},
                                  {"indices", indices},
                                  {"joints", joints},
                                  {"inverseBind", ib},
                                  {"color", m.color.name(QColor::HexArgb)},
                                  {"targets", targets},
                                  {"textures", textures},
                                  {"metallic", m.metallic},
                                  {"roughness", m.roughness},
                                  {"emissive", vec(m.emissive)},
                                  {"alphaMode", m.alphaMode},
                                  {"alphaCutoff", m.alphaCutoff},
                                  {"doubleSided", m.doubleSided},
                                  {"unlit", m.unlit},
                                  {"normalScale", m.normalScale},
                                  {"occlusionStrength", m.occlusionStrength}});
    }
    for (auto it = s.keys.cbegin(); it != s.keys.cend(); ++it)
        keys.append(QJsonObject{{"frame", it.key()},{"id",it->id},
                                {"pose", poseJson(it->pose)},
                                {"pins", pinJson(it->pins)},
                                {"morphs", morphJson(it->morphs)}});
    QJsonObject roles, mirrors;
    QJsonArray chains;
    for (auto it = s.profile.roles.cbegin(); it != s.profile.roles.cend(); ++it)
        roles[it.key()] = it.value();
    for (auto it = s.profile.mirrors.cbegin(); it != s.profile.mirrors.cend(); ++it)
        mirrors[QString::number(it.key())] = it.value();
    for (auto chain : s.profile.chains)
        chains.append(QJsonObject{
            {"root", chain.root}, {"mid", chain.mid}, {"end", chain.end}, {"pole", vec(chain.pole)}});
    QJsonObject profile{
        {"configured", s.profile.configured}, {"roles", roles}, {"mirrors", mirrors}, {"chains", chains}};
    QJsonArray clips;
    if (s.clips)
        for (const auto &clip : *s.clips) {
            QJsonArray channels, samples;
            for (const auto &channel : clip.channels)
                channels.append(QJsonObject{{"node", channel.node},
                                            {"path", channel.path},
                                            {"interpolation", channel.interpolation},
                                            {"components", channel.components},
                                            {"times", floats(channel.times)},
                                            {"values", floats(channel.values)}});
            if (clip.channels.empty())
                for (auto it = clip.samples.cbegin(); it != clip.samples.cend(); ++it)
                    samples.append(QJsonObject{{"frame", it.key()},
                                               {"pose", poseJson(it->pose)},
                                               {"pins", pinJson(it->pins)},
                                               {"morphs", morphJson(it->morphs)}});
            clips.append(QJsonObject{{"name", clip.name},
                                     {"duration", clip.duration},
                                     {"fps", clip.sampleFps},
                                     {"channels", channels},
                                     {"samples", samples}});
        }
    QJsonArray layers;
    for(const auto &layer:s.layers) {
    QJsonArray removedPins;
    for (auto n : layer.removedPins)
        removedPins.append(n);
    QJsonArray offsets;
    for (auto it = layer.offsets.cbegin(); it != layer.offsets.cend(); ++it)
        offsets.append(QJsonObject{{"node", it.key()}, {"offset", tr(it.value())}});
    QJsonArray layerKeys;
    for(auto key=layer.keys.cbegin();key!=layer.keys.cend();++key){
        QJsonArray keyedOffsets;
        for(auto it=key->offsets.cbegin();it!=key->offsets.cend();++it)keyedOffsets.append(QJsonObject{{"node",it.key()},{"offset",tr(it.value())},{"channels",key->channels.value(it.key(),7)}});
        layerKeys.append(QJsonObject{{"frame",key.key()},{"id",key->id},{"offsets",keyedOffsets},{"morphOffsets",morphJson(key->morphOffsets)}});
    }
    QJsonObject correction{{"present", layer.present},
                           {"enabled", layer.enabled},
                           {"editing", layer.editing},
                           {"world", layer.world},
                           {"first", layer.first},
                           {"last", layer.last},
                           {"fadeIn", layer.fadeIn},
                           {"fadeOut", layer.fadeOut},
                           {"strength", layer.strength},
                           {"basePose", poseJson(layer.basePose)},
                           {"baseMorphs", morphJson(layer.baseMorphs)},
                           {"offsets", offsets},
                           {"morphOffsets", morphJson(layer.morphOffsets)},
                           {"basePins", pinJson(layer.basePins)},
                           {"pins", pinJson(layer.pins)},
                           {"removedPins", removedPins}};
    correction["id"]=layer.id;correction["name"]=layer.name;correction["keys"]=layerKeys;
    layers.append(correction);
    }
    QJsonArray bodies;
    for (const auto &b : s.bodies)
        bodies.append(QJsonObject{{"node", b.node},
                                  {"mass", b.mass},
                                  {"radius", b.radius},
                                  {"center", vec(b.center)},
                                  {"end", vec(b.end)}, {"inertia",vec(b.inertia)}, {"inertiaRotation",QJsonArray{b.inertiaRotation.x(),b.inertiaRotation.y(),b.inertiaRotation.z(),b.inertiaRotation.scalar()}},
 {"footHalfSize",vec(b.footHalfSize)},{"shapeRotation",QJsonArray{b.shapeRotation.x(),b.shapeRotation.y(),b.shapeRotation.z(),b.shapeRotation.scalar()}}, {"estimated",b.estimated}, {"stiffness",b.stiffness}, {"damping",b.damping}, {"maxTorque",b.maxTorque}});
    const auto &p = s.physics;
    QJsonObject physics{{"metersPerUnit", p.metersPerUnit},
                        {"gravity", p.gravity},
                        {"ground", p.ground},
                        {"friction", p.friction},
                        {"restitution", p.restitution},
                        {"substeps", p.substeps},
                        {"iterations", p.iterations},
                        {"selfCollision", p.selfCollision},
                        {"followAnimation", p.followAnimation},
                        {"stiffness", p.stiffness},
                        {"damping", p.damping},
                        {"maxTorque", p.maxTorque},
                        {"bodies", bodies}, {"backend","MuJoCo"}, {"backendVersion","3.3.7"},
 {"requests",p.requests},{"contactTime",p.contactTime},{"contactDamping",p.contactDamping},{"externalAnchors",p.externalAnchors},
 {"migrationWarnings",QJsonArray::fromStringList(p.migrationWarnings)}};
    if(p.hasJointModel) {
        QJsonArray directed,ball;
        for(auto it=p.modelJointLimits.cbegin();it!=p.modelJointLimits.cend();++it) {
            const auto &l=it.value();directed.append(QJsonObject{{"node",it.key()},{"hinge",l.hinge},
                {"axis",vec(l.axis)},{"swing",l.swing},{"minimum",l.minimum},{"maximum",l.maximum}});
        }
        for(auto it=p.modelRotationLimits.cbegin();it!=p.modelRotationLimits.cend();++it)
            ball.append(QJsonObject{{"node",it.key()},{"angle",it.value()}});
        physics["jointModel"]=QJsonObject{{"version",1},{"directed",directed},{"ball",ball}};
    }
    QJsonArray limits, contacts, jointLimits;
    for (auto it = s.jointLimits.cbegin(); it != s.jointLimits.cend(); ++it) {
        const auto &l = it.value();
        jointLimits.append(QJsonObject{{"node", it.key()},
                                       {"hinge", l.hinge},
                                       {"axis", vec(l.axis)},
                                       {"swing", l.swing},
                                       {"minimum", l.minimum},
                                       {"maximum", l.maximum}});
    }
    for (auto it = s.rotationLimits.cbegin(); it != s.rotationLimits.cend(); ++it)
        limits.append(QJsonObject{{"node", it.key()}, {"angle", it.value()}});
    for (const auto &c : s.contacts)
        contacts.append(QJsonObject{{"node", c.node},
                                    {"first", c.first},
                                    {"last", c.last},
                                    {"target", vec(c.target)},
                                    {"pole", vec(c.pole)},
                                    {"orient", c.orient},
                                    {"fadeIn", c.fadeIn},
                                    {"fadeOut", c.fadeOut},
                                    {"orientation", QJsonArray{c.orientation.x(), c.orientation.y(),
                                                               c.orientation.z(), c.orientation.scalar()}}});
    return {{"playhead", s.sampleFrame},
            {"physics", physics},{"physicsResult",s.physicsResult},{"annotations",s.annotations},{"retargetProfile",s.retargetProfile},{"curves",curvesJson(s.curves)},{"curveBasePose",poseJson(s.curveBasePose)},{"curveBaseMorphs",morphJson(s.curveBaseMorphs)},{"curveBasePins",pinJson(s.curveBasePins)},
            {"bodyAssist", s.bodyAssist},
            {"limits", limits},
            {"jointLimits", jointLimits},
            {"contacts", contacts},
            {"format", "Trickster"},
            {"version", 8},
            {"layers", layers},
            {"activeLayer",s.activeLayer},
            {"compareOriginal",s.compareOriginal},
            {"profile", profile},
            {"sourceClips", clips},
            {"sourceClip", s.sourceClip},
            {"name", s.name},
            {"nodes", nodes},
            {"meshes", meshes},
            {"pose", poseJson(s.pose)},
            {"morphs", morphJson(s.morphs)},
            {"pins", pinJson(s.pins)},
            {"keys", keys},
            {"first", s.first},
            {"last", s.last},
            {"fps", s.fps}};
}
bool sceneFromJson(const QJsonObject &o, Scene &scene, QString &error) {
    try {
        need(o["format"] == "Trickster" && (o["version"].toInt() >= 1 && o["version"].toInt() <= 8),
             "Unsupported Trickster project version.");
        Scene s;
        s.name = o["name"].toString();
        s.first = o["first"].toInt();
        s.last = o["last"].toInt(90);
        s.fps = o["fps"].toInt(30);
        for (auto v : o["nodes"].toArray()) {
            auto n = v.toObject();
            s.nodes.push_back({n["name"].toString(), n["parent"].toInt(-1), readTr(n["rest"].toObject()),
                               n["joint"].toBool(true), readFloats(n["morphRest"].toArray())});
        }
        auto profile = o["profile"].toObject();
        s.profile.configured = profile["configured"].toBool();
        auto roles = profile["roles"].toObject(), mirrors = profile["mirrors"].toObject();
        for (auto it = roles.begin(); it != roles.end(); ++it)
            s.profile.roles[it.key()] = it.value().toInt(-1);
        for (auto it = mirrors.begin(); it != mirrors.end(); ++it)
            s.profile.mirrors[it.key().toInt()] = it.value().toInt(-1);
        for (auto value : profile["chains"].toArray()) {
            auto c = value.toObject();
            IKChain chain{c["root"].toInt(-1), c["mid"].toInt(-1), c["end"].toInt(-1),
                          v3(c["pole"].toArray())};
            s.profile.chains[chain.end] = chain;
        }
        auto sourceClips = std::make_shared<std::vector<AnimationClip>>();
        for (auto value : o["sourceClips"].toArray()) {
            auto item = value.toObject();
            AnimationClip clip;
            clip.name = item["name"].toString();
            clip.duration = float(item["duration"].toDouble());
            clip.sampleFps = item["fps"].toInt(30);
            for (auto channel : item["channels"].toArray()) {
                auto ch = channel.toObject();
                clip.channels.push_back({ch["node"].toInt(-1), ch["components"].toInt(),
                                         ch["path"].toString(), ch["interpolation"].toString(),
                                         readFloats(ch["times"].toArray()),
                                         readFloats(ch["values"].toArray())});
            }
            for (auto sample : item["samples"].toArray()) {
                auto k = sample.toObject();
                clip.samples[k["frame"].toInt()] = {readPose(k["pose"].toArray()),
                                                    readPins(k["pins"].toArray()),
                                                    readMorphs(k["morphs"].toArray())};
            }
            sourceClips->push_back(std::move(clip));
        }
        if (!sourceClips->empty())
            s.clips = sourceClips;
        s.sourceClip = o["sourceClip"].toInt(-1);
        QJsonArray savedLayers=o["version"].toInt()>=4?o["layers"].toArray():QJsonArray();
        if(o["version"].toInt()<4 && o["correction"].toObject()["present"].toBool()) savedLayers.append(o["correction"]);
        need(savedLayers.size()<=32,"Too many correction layers.");
        for(auto saved:savedLayers) {
        auto correction=saved.toObject();MotionCorrection layer;
        layer.id=correction["id"].toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
        layer.name=correction["name"].toString("Migrated correction");
        layer.present = correction["present"].toBool();
        layer.enabled = correction["enabled"].toBool(true);
        layer.editing = correction["editing"].toBool(true);
        layer.world = correction["world"].toBool();
        layer.first = correction["first"].toInt();
        layer.last = correction["last"].toInt(30);
        layer.fadeIn = correction["fadeIn"].toInt(5);
        layer.fadeOut = correction["fadeOut"].toInt(5);
        layer.strength = float(correction["strength"].toDouble(1));
        layer.basePose = readPose(correction["basePose"].toArray());
        layer.baseMorphs = readMorphs(correction["baseMorphs"].toArray());
        layer.morphOffsets = readMorphs(correction["morphOffsets"].toArray());
        layer.basePins = readPins(correction["basePins"].toArray());
        layer.pins = readPins(correction["pins"].toArray());
        for (auto value : correction["removedPins"].toArray())
            layer.removedPins.insert(value.toInt(-1));
        for (auto value : correction["offsets"].toArray()) {
            auto item = value.toObject();
            layer.offsets[item["node"].toInt(-1)] = readTr(item["offset"].toObject());
        }
        for(auto savedKey:correction["keys"].toArray()){
            auto key=savedKey.toObject();int frame=key["frame"].toInt(-1);
            need(!layer.keys.contains(frame),"Duplicate correction key time.");
            auto &entry=layer.keys[frame];entry.id=key["id"].toString();entry.morphOffsets=readMorphs(key["morphOffsets"].toArray());
            for(auto offset:key["offsets"].toArray()){
                auto value=offset.toObject();int node=value["node"].toInt(-1);
                need(!entry.offsets.contains(node),"Duplicate correction channel.");
                entry.offsets[node]=readTr(value["offset"].toObject());
                if(value.contains("channels"))entry.channels[node]=value["channels"].toInt();
            }
        }
        s.layers.push_back(std::move(layer));
        }
        s.activeLayer=s.layers.empty()?-1:o["activeLayer"].toInt(0);
        s.compareOriginal=o["compareOriginal"].toBool();
        s.sampleFrame = float(o["playhead"].toDouble(s.first));
        s.bodyAssist = o["bodyAssist"].toBool();
        s.physicsResult=o["physicsResult"].toObject();
        need(!o.contains("annotations") || o["annotations"].isArray(),"Invalid annotations array.");
        s.annotations=o["annotations"].toArray();
        s.retargetProfile=o["retargetProfile"].toObject();
        s.curves=readCurves(o["curves"].toArray());
        s.curveBasePose=readPose(o["curveBasePose"].toArray());
        s.curveBaseMorphs=readMorphs(o["curveBaseMorphs"].toArray());s.curveBasePins=readPins(o["curveBasePins"].toArray());
        auto physics = o["physics"].toObject();
        auto &p = s.physics;
        p.metersPerUnit = float(physics["metersPerUnit"].toDouble(1));
        p.gravity = float(physics["gravity"].toDouble(9.81));
        p.ground = float(physics["ground"].toDouble());
        p.friction = float(physics["friction"].toDouble(.8));
        p.restitution = float(physics["restitution"].toDouble());
        p.substeps = physics["substeps"].toInt(8);
        p.iterations = physics["iterations"].toInt(80);
        p.selfCollision = physics["selfCollision"].toBool();
        p.followAnimation = physics["followAnimation"].toBool();
        p.stiffness = float(physics["stiffness"].toDouble(80));
        p.damping = float(physics["damping"].toDouble(8));
        p.maxTorque = float(physics["maxTorque"].toDouble(100));
        p.requests=physics["requests"].toObject();
        p.contactTime=float(physics["contactTime"].toDouble(.01));
        p.contactDamping=float(physics["contactDamping"].toDouble(1));
        p.externalAnchors=physics["externalAnchors"].toBool(true);
        for(auto w:physics["migrationWarnings"].toArray())p.migrationWarnings<<w.toString();
        if(o["version"].toInt()<5) p.migrationWarnings<<"Migrated Bullet project: masses/shapes/PD units retained; restitution is archival, contacts use MuJoCo time/damping. Authored pins/contacts are external anchors only in legacy preview. Recalculation differs; baked keys are preserved.";
        if(physics.contains("jointModel")) {
            auto model=physics["jointModel"].toObject();need(model["version"].toInt()==1,"Unsupported physical joint model version.");
            p.hasJointModel=true;
            for(auto value:model["directed"].toArray()) {
                auto l=value.toObject();int node=l["node"].toInt(-1);need(!p.modelJointLimits.contains(node),"Duplicate physical joint definition.");
                p.modelJointLimits[node]={l["hinge"].toBool(),v3(l["axis"].toArray()),float(l["swing"].toDouble()),float(l["minimum"].toDouble()),float(l["maximum"].toDouble())};
            }
            for(auto value:model["ball"].toArray()) {
                auto l=value.toObject();int node=l["node"].toInt(-1);need(!p.modelRotationLimits.contains(node),"Duplicate physical ball limit.");
                p.modelRotationLimits[node]=float(l["angle"].toDouble());
            }
        }
        if(o["version"].toInt()<6 && s.physicsResult.contains("controlReplay") && !p.hasJointModel)
            p.migrationWarnings<<"Legacy baked physics report lacks a separate physical joint model. Its frozen replay is retained; recalculate from the original source to obtain new baked-motion validation.";
        for (auto v : physics["bodies"].toArray()) {
            auto b = v.toObject();
            s.bodies.push_back({b["node"].toInt(-1), float(b["mass"].toDouble()),
                                float(b["radius"].toDouble()), v3(b["center"].toArray()),
                                v3(b["end"].toArray())});
            auto &d=s.bodies.back();d.inertia=v3(b["inertia"].toArray());
            if(b.contains("inertiaRotation")){auto r=b["inertiaRotation"].toArray();need(r.size()==4,"Invalid inertia quaternion");d.inertiaRotation=QQuaternion(float(r[3].toDouble()),float(r[0].toDouble()),float(r[1].toDouble()),float(r[2].toDouble()));}
            d.footHalfSize=v3(b["footHalfSize"].toArray());
            if(b.contains("shapeRotation")){auto q=b["shapeRotation"].toArray();need(q.size()==4,"Invalid shape rotation");d.shapeRotation=QQuaternion(float(q[3].toDouble()),float(q[0].toDouble()),float(q[1].toDouble()),float(q[2].toDouble()));}d.estimated=b["estimated"].toBool(true);
            d.stiffness=float(b["stiffness"].toDouble(-1));d.damping=float(b["damping"].toDouble(-1));d.maxTorque=float(b["maxTorque"].toDouble(-1));
        }
        for (auto value : o["jointLimits"].toArray()) {
            auto l = value.toObject();
            s.jointLimits[l["node"].toInt(-1)] = {
                l["hinge"].toBool(), v3(l["axis"].toArray()), float(l["swing"].toDouble()),
                float(l["minimum"].toDouble()), float(l["maximum"].toDouble())};
        }
        for (auto v : o["limits"].toArray()) {
            auto l = v.toObject();
            s.rotationLimits[l["node"].toInt(-1)] = float(l["angle"].toDouble());
        }
        for (auto v : o["contacts"].toArray()) {
            auto c = v.toObject();
            s.contacts.push_back({c["node"].toInt(-1), c["first"].toInt(), c["last"].toInt(),
                                  v3(c["target"].toArray()), v3(c["pole"].toArray())});
            auto &contact = s.contacts.back();
            auto q = c["orientation"].toArray();
            if (q.size() == 4)
                contact.orientation = QQuaternion(float(q[3].toDouble()), float(q[0].toDouble()),
                                                  float(q[1].toDouble()), float(q[2].toDouble()));
            contact.orient = c["orient"].toBool();
            contact.fadeIn = c["fadeIn"].toInt();
            contact.fadeOut = c["fadeOut"].toInt();
        }
        s.pose = readPose(o["pose"].toArray());
        s.morphs = readMorphs(o["morphs"].toArray());
        s.pins = readPins(o["pins"].toArray());
        for (auto v : o["keys"].toArray()) {
            auto k = v.toObject();
            s.keys[k["frame"].toInt()] = {readPose(k["pose"].toArray()), readPins(k["pins"].toArray()),
                                          readMorphs(k["morphs"].toArray()),k["id"].toString()};
        }
        for (auto v : o["meshes"].toArray()) {
            auto m = v.toObject();
            Mesh mesh;
            mesh.node = m["node"].toInt(-1);
            mesh.color = QColor(m["color"].toString("#6d9cb3"));
            mesh.metallic = float(m["metallic"].toDouble());
            mesh.roughness = float(m["roughness"].toDouble(.75));
            mesh.emissive = v3(m["emissive"].toArray());
            mesh.alphaMode = m["alphaMode"].toString("OPAQUE");
            mesh.alphaCutoff = float(m["alphaCutoff"].toDouble(.5));
            mesh.doubleSided = m["doubleSided"].toBool(true);
            mesh.unlit = m["unlit"].toBool();
            mesh.normalScale = float(m["normalScale"].toDouble(1));
            mesh.occlusionStrength = float(m["occlusionStrength"].toDouble(1));
            for (auto targetValue : m["targets"].toArray()) {
                auto t = targetValue.toObject();
                mesh.targets.push_back({t["name"].toString(), readVectors(t["positions"].toArray()),
                                        readVectors(t["normals"].toArray()),
                                        readVectors(t["tangents"].toArray())});
            }
            auto textures = m["textures"].toObject();
            for (auto it = textures.begin(); it != textures.end(); ++it) {
                auto t = it.value().toObject();
                mesh.textures[it.key()] = {QByteArray::fromBase64(t["bytes"].toString().toLatin1()),
                                           t["mime"].toString(),
                                           t["coord"].toInt(),
                                           t["wrapS"].toInt(10497),
                                           t["wrapT"].toInt(10497),
                                           t["minFilter"].toInt(9987),
                                           t["magFilter"].toInt(9729)};
                auto &tex = mesh.textures[it.key()];
                auto offset = t["offset"].toArray(), scale = t["scale"].toArray();
                if (offset.size() == 2)
                    tex.offset = {float(offset[0].toDouble()), float(offset[1].toDouble())};
                if (scale.size() == 2)
                    tex.scale = {float(scale[0].toDouble()), float(scale[1].toDouble())};
                tex.rotation = float(t["rotation"].toDouble());
            }
            for (auto vv : m["vertices"].toArray()) {
                auto a = vv.toArray();
                need(a.size() == 11 || a.size() == 26 || a.size() == 30, "Invalid project vertex.");
                Vertex vt;
                vt.p = v3(QJsonArray{a[0], a[1], a[2]});
                int count = a.size() == 11 ? 4 : 8;
                for (int k = 0; k < count; ++k) {
                    vt.joints[k] = a[3 + k].toInt();
                    vt.weights[k] = float(a[3 + count + k].toDouble());
                }
                if (a.size() == 30)
                    vt.tangent = {float(a[26].toDouble()), float(a[27].toDouble()), float(a[28].toDouble()),
                                  float(a[29].toDouble())};
                if (count == 8) {
                    vt.normal = v3(QJsonArray{a[19], a[20], a[21]});
                    vt.uv = {float(a[22].toDouble()), float(a[23].toDouble())};
                    vt.uv1 = {float(a[24].toDouble()), float(a[25].toDouble())};
                }
                mesh.vertices.push_back(vt);
            }
            for (auto i : m["indices"].toArray())
                mesh.indices.push_back(unsigned(i.toInt(-1)));
            for (auto j : m["joints"].toArray())
                mesh.joints.push_back(j.toInt(-1));
            for (auto mv : m["inverseBind"].toArray()) {
                auto a = mv.toArray();
                need(a.size() == 16, "Invalid inverse bind matrix.");
                QMatrix4x4 mat;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        mat(r, c) = float(a[c * 4 + r].toDouble());
                mesh.inverseBind.push_back(mat);
            }
            s.meshes.push_back(std::move(mesh));
        }
        need(s.validate(&error), error);
        scene = std::move(s);
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
bool saveProject(const QString &path, const Scene &s, QString &error) {
    if (!s.validate(&error))
        return false;
    return writeFile(path, QJsonDocument(sceneToJson(s)).toJson(QJsonDocument::Compact), error);
}
bool loadProject(const QString &path, Scene &s, QString &error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        error = f.errorString();
        return false;
    }
    if (f.size() > 512 * 1024 * 1024) {
        error = "Project exceeds 512 MB limit.";
        return false;
    }
    QJsonParseError parse;
    auto doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError) {
        error = parse.errorString();
        return false;
    }
    return sceneFromJson(doc.object(), s, error);
}
bool loadGltf(const QString &path, Scene &scene, QString &error) {
    try {
        QFile file(path);
        need(file.open(QIODevice::ReadOnly), file.errorString());
        need(file.size() <= 512 * 1024 * 1024, "File exceeds 512 MB limit.");
        QByteArray bytes = file.readAll(), json, bin;
        if (bytes.size() >= 4 && u32(bytes, 0) == 0x46546c67) {
            need(u32(bytes, 4) == 2 && u32(bytes, 8) == quint32(bytes.size()), "Invalid GLB header.");
            qsizetype at = 12;
            while (at < bytes.size()) {
                quint32 size = u32(bytes, at), type = u32(bytes, at + 4);
                at += 8;
                need(at + size <= bytes.size(), "Truncated GLB chunk.");
                if (type == 0x4e4f534a)
                    json = bytes.mid(at, size);
                else if (type == 0x004e4942)
                    bin = bytes.mid(at, size);
                at += size;
            }
        } else
            json = bytes;
        QJsonParseError parse;
        auto doc = QJsonDocument::fromJson(json, &parse);
        need(parse.error == QJsonParseError::NoError && doc.isObject(),
             "Invalid glTF JSON: " + parse.errorString());
        Reader reader;
        reader.root = doc.object();
        auto &root = reader.root;
        need(root["asset"].toObject()["version"].toString() == "2.0", "Only glTF 2.0 is supported.");
        for (auto ext : root["extensionsRequired"].toArray())
            need((ext.toString() == "KHR_materials_unlit" || ext.toString() == "KHR_texture_transform"),
                 "Unsupported required extension: " + ext.toString());
        for (auto b : root["buffers"].toArray()) {
            auto o = b.toObject();
            QString uri = o["uri"].toString();
            QByteArray buffer;
            if (uri.isEmpty())
                buffer = bin;
            else if (uri.startsWith("data:")) {
                int comma = uri.indexOf(',');
                need(comma > 0 && uri.left(comma).endsWith(";base64"),
                     "Only base64 data URIs are supported.");
                buffer = QByteArray::fromBase64(uri.mid(comma + 1).toLatin1());
            } else {
                QFile external(QFileInfo(path).dir().filePath(QUrl::fromPercentEncoding(uri.toUtf8())));
                need(external.open(QIODevice::ReadOnly), "Missing buffer: " + uri);
                need(external.size() <= 512 * 1024 * 1024, "External buffer exceeds 512 MB.");
                buffer = external.readAll();
            }
            need(buffer.size() >= o["byteLength"].toInteger(), "Truncated glTF buffer.");
            reader.buffers.push_back(std::move(buffer));
        }
        Scene s;
        s.fps = std::clamp(root["asset"].toObject()["extras"].toObject()["tricksterFPS"].toInt(30), 1, 240);
        s.name = QFileInfo(path).completeBaseName();
        auto ns = root["nodes"].toArray();
        need(ns.size() > 0 && ns.size() <= 4096, "glTF must contain 1-4096 nodes.");
        for (int i = 0; i < ns.size(); ++i) {
            auto n = ns[i].toObject();
            s.nodes.push_back({n["name"].toString(QString("Node %1").arg(i)), -1, nodeTransform(n), false});
        }
        for (int i = 0; i < ns.size(); ++i)
            for (auto child : ns[i].toObject()["children"].toArray()) {
                int c = child.toInt(-1);
                need(c >= 0 && c < int(s.nodes.size()) && s.nodes[c].parent == -1, "Invalid node hierarchy.");
                s.nodes[c].parent = i;
            }
        s.pose = s.restPose();
        need(s.validate(&error), error);
        auto skins = root["skins"].toArray();
        for (auto sv : skins)
            for (auto j : sv.toObject()["joints"].toArray()) {
                int idx = j.toInt(-1);
                need(idx >= 0 && idx < int(s.nodes.size()), "Skin joint out of range.");
                s.nodes[idx].joint = true;
            }
        if (skins.isEmpty()) {
            for (auto &n : s.nodes)
                n.joint = true;
            s.warnings << "No skin found: nodes can be animated, mesh is rigid.";
        }
        auto readTexture = [&](QJsonObject info) {
            auto textures = root["textures"].toArray(), images = root["images"].toArray(),
                 samplers = root["samplers"].toArray();
            int ti = info["index"].toInt(-1);
            need(ti >= 0 && ti < textures.size(), "Invalid texture index.");
            auto t = textures[ti].toObject();
            int ii = t["source"].toInt(-1);
            need(ii >= 0 && ii < images.size(), "Invalid image index.");
            auto im = images[ii].toObject();
            Texture out;
            out.coord = info["texCoord"].toInt();
            out.mime = im["mimeType"].toString();
            QString uri = im["uri"].toString();
            if (im.contains("bufferView")) {
                auto views = root["bufferViews"].toArray();
                int vi = im["bufferView"].toInt(-1);
                need(vi >= 0 && vi < views.size(), "Invalid image buffer view.");
                auto v = views[vi].toObject();
                int bi = v["buffer"].toInt(-1);
                qint64 at = v["byteOffset"].toInteger(), len = v["byteLength"].toInteger();
                need(bi >= 0 && bi < int(reader.buffers.size()) && at >= 0 && len > 0 &&
                         len <= 64 * 1024 * 1024 && at + len <= reader.buffers[bi].size(),
                     "Invalid image buffer range.");
                out.bytes = reader.buffers[bi].mid(at, len);
            } else if (uri.startsWith("data:")) {
                int comma = uri.indexOf(',');
                need(comma > 0 && uri.left(comma).endsWith(";base64"), "Invalid image data URI.");
                out.mime = uri.mid(5, uri.indexOf(';') - 5);
                out.bytes = QByteArray::fromBase64(uri.mid(comma + 1).toLatin1());
            } else {
                QFile f(QFileInfo(path).dir().filePath(QUrl::fromPercentEncoding(uri.toUtf8())));
                need(f.open(QIODevice::ReadOnly), "Missing texture: " + uri);
                need(f.size() <= 64 * 1024 * 1024, "Image exceeds 64 MB.");
                out.bytes = f.readAll();
            }
            QBuffer buf(&out.bytes);
            buf.open(QIODevice::ReadOnly);
            QImageReader decoder(&buf);
            auto size = decoder.size();
            need(size.isValid() && qint64(size.width()) * size.height() <= 64000000,
                 "Unsupported or oversized texture image.");
            need(decoder.canRead(), "Cannot decode texture.");
            if (out.mime.isEmpty())
                out.mime = decoder.format() == "jpeg" ? "image/jpeg" : "image/png";
            if (t.contains("sampler")) {
                int si = t["sampler"].toInt(-1);
                need(si >= 0 && si < samplers.size(), "Invalid texture sampler.");
                auto sp = samplers[si].toObject();
                out.wrapS = sp["wrapS"].toInt(10497);
                out.wrapT = sp["wrapT"].toInt(10497);
                out.minFilter = sp["minFilter"].toInt(9987);
                out.magFilter = sp["magFilter"].toInt(9729);
            }
            need(out.coord >= 0 && out.coord <= 1, "Only UV sets 0 and 1 are supported.");
            auto transform = info["extensions"].toObject()["KHR_texture_transform"].toObject();
            auto offset = transform["offset"].toArray(), scale = transform["scale"].toArray();
            if (offset.size() == 2)
                out.offset = {float(offset[0].toDouble()), float(offset[1].toDouble())};
            if (scale.size() == 2)
                out.scale = {float(scale[0].toDouble()), float(scale[1].toDouble())};
            out.rotation = float(transform["rotation"].toDouble());
            if (transform.contains("texCoord"))
                out.coord = transform["texCoord"].toInt();
            need(out.coord >= 0 && out.coord <= 1, "Texture transform references unsupported UV set.");
            return out;
        };
        auto ms = root["meshes"].toArray(), materials = root["materials"].toArray();
        for (int ni = 0; ni < ns.size(); ++ni) {
            auto no = ns[ni].toObject();
            if (!no.contains("mesh"))
                continue;
            int mi = no["mesh"].toInt(-1);
            need(mi >= 0 && mi < ms.size(), "Mesh index out of range.");
            for (auto pv : ms[mi].toObject()["primitives"].toArray()) {
                auto prim = pv.toObject();
                need(prim["mode"].toInt(4) == 4, "Only triangle primitives are supported.");
                need(!prim["extensions"].toObject().contains("KHR_draco_mesh_compression"),
                     "Draco compression is not supported. Export uncompressed glTF.");
                auto attrs = prim["attributes"].toObject();
                auto positions = reader.accessor(attrs["POSITION"].toInt(-1));
                need(positions.components == 3, "POSITION must be VEC3.");
                Mesh m;
                m.node = ni;
                m.color = Qt::white;
                auto meshObj = ms[mi].toObject();
                auto targets = prim["targets"].toArray();
                auto names = meshObj["extras"].toObject()["targetNames"].toArray();
                auto defaults =
                    no.contains("weights") ? no["weights"].toArray() : meshObj["weights"].toArray();
                if (!targets.isEmpty()) {
                    need(defaults.isEmpty() || defaults.size() == targets.size(), "Invalid morph defaults.");
                    s.nodes[ni].morphRest =
                        defaults.isEmpty() ? std::vector<float>(targets.size(), 0) : readFloats(defaults);
                    for (int k = 0; k < targets.size(); ++k) {
                        MorphTarget target;
                        target.name = k < names.size() ? names[k].toString() : QString("Shape %1").arg(k + 1);
                        auto t = targets[k].toObject();
                        for (const auto &prop : {QString("POSITION"), QString("NORMAL"), QString("TANGENT")})
                            if (t.contains(prop)) {
                                auto a = reader.accessor(t[prop].toInt(-1));
                                need(a.count == positions.count && a.components == 3,
                                     "Invalid morph target accessor.");
                                auto &dest = prop == "POSITION" ? target.positions
                                             : prop == "NORMAL" ? target.normals
                                                                : target.tangents;
                                for (int i = 0; i < a.count; ++i)
                                    dest.push_back({a.at(i, 0), a.at(i, 1), a.at(i, 2)});
                            }

                        m.targets.push_back(std::move(target));
                    }
                }
                if (prim.contains("material")) {
                    int mat = prim["material"].toInt(-1);
                    need(mat >= 0 && mat < materials.size(), "Material index out of range.");
                    auto pbr = materials[mat].toObject()["pbrMetallicRoughness"].toObject();
                    auto color = pbr["baseColorFactor"].toArray();
                    if (color.size() == 4)
                        m.color = QColor::fromRgbF(
                            std::clamp(color[0].toDouble(), 0., 1.), std::clamp(color[1].toDouble(), 0., 1.),
                            std::clamp(color[2].toDouble(), 0., 1.), std::clamp(color[3].toDouble(), 0., 1.));
                    auto material = materials[mat].toObject();
                    m.metallic = float(pbr["metallicFactor"].toDouble(1));
                    m.roughness = float(pbr["roughnessFactor"].toDouble(1));
                    m.emissive = v3(material["emissiveFactor"].toArray());
                    m.alphaMode = material["alphaMode"].toString("OPAQUE");
                    m.alphaCutoff = float(material["alphaCutoff"].toDouble(.5));
                    m.doubleSided = material["doubleSided"].toBool();
                    m.unlit = material["extensions"].toObject().contains("KHR_materials_unlit");
                    for (const auto &slot : {QString("baseColor"), QString("metallicRoughness"),
                                             QString("normal"), QString("occlusion"), QString("emissive")}) {
                        auto container =
                            (slot == "baseColor" || slot == "metallicRoughness") ? pbr : material;
                        QString prop = slot + "Texture";
                        if (container.contains(prop))
                            m.textures[slot] = readTexture(container[prop].toObject());
                    }
                    m.normalScale = float(material["normalTexture"].toObject()["scale"].toDouble(1));
                    m.occlusionStrength =
                        float(material["occlusionTexture"].toObject()["strength"].toDouble(1));
                }
                Access joints, weights, joints1, weights1, normal, uv, uv1, tangent;
                auto attribute = [&](const char *name, int components, Access &a) {
                    if (attrs.contains(name)) {
                        a = reader.accessor(attrs[name].toInt(-1));
                        need(a.count == positions.count && a.components == components,
                             "Invalid vertex attribute.");
                    }
                };
                attribute("NORMAL", 3, normal);
                attribute("TANGENT", 4, tangent);
                attribute("TEXCOORD_0", 2, uv);
                attribute("TEXCOORD_1", 2, uv1);
                bool skinned = no.contains("skin");
                if (skinned) {
                    int si = no["skin"].toInt(-1);
                    need(si >= 0 && si < skins.size(), "Skin index out of range.");
                    auto sk = skins[si].toObject();
                    for (auto j : sk["joints"].toArray())
                        m.joints.push_back(j.toInt(-1));
                    Access ib;
                    if (sk.contains("inverseBindMatrices")) {
                        ib = reader.accessor(sk["inverseBindMatrices"].toInt(-1));
                        need(ib.components == 16 && ib.count >= int(m.joints.size()),
                             "Invalid inverse bind matrices.");
                    }
                    for (int i = 0; i < int(m.joints.size()); ++i) {
                        QMatrix4x4 mat;
                        if (ib.count)
                            for (int c = 0; c < 4; ++c)
                                for (int r = 0; r < 4; ++r)
                                    mat(r, c) = ib.at(i, c * 4 + r);
                        m.inverseBind.push_back(mat);
                    }
                    joints = reader.accessor(attrs["JOINTS_0"].toInt(-1));
                    weights = reader.accessor(attrs["WEIGHTS_0"].toInt(-1));
                    need(joints.count == positions.count && weights.count == positions.count &&
                             joints.components == 4 && weights.components == 4,
                         "Invalid skin attributes.");
                    if (attrs.contains("JOINTS_1")) {
                        attribute("JOINTS_1", 4, joints1);
                        attribute("WEIGHTS_1", 4, weights1);
                        need(weights1.count > 0, "Missing WEIGHTS_1.");
                    }
                }
                for (int i = 0; i < positions.count; ++i) {
                    Vertex v;
                    v.p = {positions.at(i, 0), positions.at(i, 1), positions.at(i, 2)};
                    if (tangent.count)
                        v.tangent = {tangent.at(i, 0), tangent.at(i, 1), tangent.at(i, 2), tangent.at(i, 3)};
                    if (normal.count)
                        v.normal = {normal.at(i, 0), normal.at(i, 1), normal.at(i, 2)};
                    if (uv.count)
                        v.uv = {uv.at(i, 0), uv.at(i, 1)};
                    if (uv1.count)
                        v.uv1 = {uv1.at(i, 0), uv1.at(i, 1)};
                    if (skinned)
                        for (int k = 0; k < (joints1.count ? 8 : 4); ++k) {
                            v.joints[k] = int((k < 4 ? joints : joints1).at(i, k % 4));
                            v.weights[k] = (k < 4 ? weights : weights1).at(i, k % 4);
                            need(v.joints[k] >= 0 && v.joints[k] < int(m.joints.size()) && v.weights[k] >= 0,
                                 "Skin influence out of range.");
                        }
                    m.vertices.push_back(v);
                }
                if (prim.contains("indices")) {
                    auto ix = reader.accessor(prim["indices"].toInt(-1));
                    need(ix.components == 1, "Indices must be scalar.");
                    for (float v : ix.data) {
                        need(v >= 0 && v < positions.count && v == std::floor(v),
                             "Triangle index out of range.");
                        m.indices.push_back(unsigned(v));
                    }
                } else
                    for (unsigned i = 0; i < unsigned(positions.count); ++i)
                        m.indices.push_back(i);
                for (auto it = m.textures.cbegin(); it != m.textures.cend(); ++it)
                    need(it->coord == 0 ? uv.count > 0 : uv1.count > 0, "Texture references missing UV set.");
                s.meshes.push_back(std::move(m));
            }
        }
        // Keep authored seconds/interpolation; frame samples remain available for explicit source-key
        // editing.
        auto anims = root["animations"].toArray();
        need(anims.size() <= 256, "More than 256 animation clips.");
        auto sourceClips = std::make_shared<std::vector<AnimationClip>>();
        for (auto animation : anims) {
            auto an = animation.toObject();
            auto samplers = an["samplers"].toArray();
            struct Channel {
                int node;
                QString path, mode;
                Access times, values;
            };
            std::vector<Channel> channels;
            float duration = 0;
            for (auto cv : an["channels"].toArray()) {
                auto c = cv.toObject(), target = c["target"].toObject();
                QString prop = target["path"].toString();
                need(prop == "translation" || prop == "rotation" || prop == "scale" || prop == "weights",
                     "Unsupported animation target.");
                int ni = target["node"].toInt(-1), si = c["sampler"].toInt(-1);
                need(ni >= 0 && ni < int(ns.size()) && si >= 0 && si < samplers.size(),
                     "Animation channel out of range.");
                auto sampler = samplers[si].toObject();
                Channel ch{ni, prop, sampler["interpolation"].toString("LINEAR"),
                           reader.accessor(sampler["input"].toInt(-1)),
                           reader.accessor(sampler["output"].toInt(-1))};
                need(ch.times.components == 1 && ch.times.count > 0, "Empty animation input.");
                need(ch.mode == "LINEAR" || ch.mode == "STEP" || ch.mode == "CUBICSPLINE",
                     "Unsupported interpolation.");
                if (prop == "weights") {
                    int n = int(s.nodes[ni].morphRest.size());
                    need(n > 0 && ch.values.components == 1 &&
                             ch.values.count == ch.times.count * (ch.mode == "CUBICSPLINE" ? 3 : 1) * n,
                         "Invalid morph animation output.");
                    ch.values.components = n;
                    ch.values.count /= n;
                }
                need(ch.values.count == ch.times.count * (ch.mode == "CUBICSPLINE" ? 3 : 1) &&
                         ch.values.components == (prop == "weights"    ? int(s.nodes[ni].morphRest.size())
                                                  : prop == "rotation" ? 4
                                                                       : 3),
                     "Animation output mismatch.");
                for (int i = 0; i < ch.times.count; ++i)
                    need(ch.times.at(i) >= 0 && (i == 0 || ch.times.at(i) > ch.times.at(i - 1)),
                         "Animation times must increase.");
                duration = std::max(duration, ch.times.at(ch.times.count - 1));
                channels.push_back(std::move(ch));
            }
            need(double(duration) * s.fps * ns.size() <= 5000000,
                 "Animation is too large to sample in memory.");
            need(duration * s.fps <= 100000, "Animation exceeds 100000 frames.");
            s.last = std::max(1, int(std::ceil(duration * s.fps)));
            AnimationClip clip;
            clip.name = an["name"].toString(QString("Clip %1").arg(sourceClips->size() + 1));
            clip.duration = duration;
            clip.sampleFps = s.fps;
            for (const auto &ch : channels) {
                AnimationChannel c;
                c.node = ch.node;
                c.path = ch.path;
                c.interpolation = ch.mode;
                c.components = ch.values.components;
                for (int i = 0; i < ch.times.count; ++i)
                    c.times.push_back(ch.times.at(i));
                for (int i = 0; i < ch.values.count; ++i)
                    for (int j = 0; j < c.components; ++j)
                        c.values.push_back(ch.values.at(i, j));
                clip.channels.push_back(std::move(c));
            }
            sourceClips->push_back(std::move(clip));
            s.keys.clear();
        }
        if (!sourceClips->empty()) {
            s.clips = sourceClips;
            need(s.selectClip(0, error), error);
        }
        s.warnings.removeDuplicates();
        need(s.validate(&error), error);
        scene = std::move(s);
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}

bool exportGltf(const QString &path, const Scene &original, QString &error) {
    try {
        need(!original.nodes.empty(), "Empty scene: add or open a character before exporting animation.");
        need(original.validate(&error), error);
        need(qint64(original.last - original.first + 1) * qint64(original.nodes.size()) <= 5000000,
             "Clip is too large to bake in memory. Shorten the export range.");
        Scene s = original;
        for (auto &mesh : s.meshes)
            completeGeometryBasis(mesh);
        QJsonObject root{{"asset", QJsonObject{{"version", "2.0"},
                                               {"generator", "Trickster " TRICKSTER_VERSION},
                                               {"extras", QJsonObject{{"tricksterFPS", s.fps}}}}}};
        QJsonArray nodes, roots, meshes, skins, materials, views, accessors, images, textures,
            textureSamplers;
        std::vector<int> meshNodes;
        QByteArray bin;
        auto accessor = [&](const std::vector<float> &data, int components, QString type,
                            bool bounds = false) {
            while (bin.size() % 4)
                bin.append('\0');
            int offset = int(bin.size());
            for (float v : data)
                appendFloat(bin, v);
            int view = views.size();
            views.append(
                QJsonObject{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", int(bin.size()) - offset}});
            QJsonObject a{{"bufferView", view},
                          {"componentType", 5126},
                          {"count", int(data.size()) / components},
                          {"type", type}};
            if (bounds && !data.empty()) {
                QJsonArray lo, hi;
                for (int j = 0; j < components; ++j) {
                    float mn = data[j], mx = mn;
                    for (size_t i = j; i < data.size(); i += components) {
                        mn = std::min(mn, data[i]);
                        mx = std::max(mx, data[i]);
                    }
                    lo.append(mn);
                    hi.append(mx);
                }
                a["min"] = lo;
                a["max"] = hi;
            }
            int idx = accessors.size();
            accessors.append(a);
            return idx;
        };
        auto unsignedAccessor = [&](const std::vector<unsigned> &data, int components, QString type, int ct) {
            while (bin.size() % 4)
                bin.append('\0');
            int offset = int(bin.size());
            for (unsigned v : data) {
                if (ct == 5123) {
                    quint16 le = qToLittleEndian(quint16(v));
                    bin.append(reinterpret_cast<const char *>(&le), 2);
                } else
                    appendU32(bin, v);
            }
            int view = views.size();
            views.append(
                QJsonObject{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", int(bin.size()) - offset}});
            int idx = accessors.size();
            accessors.append(QJsonObject{{"bufferView", view},
                                         {"componentType", ct},
                                         {"count", int(data.size()) / components},
                                         {"type", type}});
            return idx;
        };
        for (int i = 0; i < int(s.nodes.size()); ++i) {
            const auto &n = s.nodes[i];
            auto t = n.rest;
            QJsonObject no{{"name", n.name},
                           {"translation", vec(t.t)},
                           {"rotation", QJsonArray{t.r.x(), t.r.y(), t.r.z(), t.r.scalar()}},
                           {"scale", vec(t.s)}};
            QJsonArray ch;
            for (int j = 0; j < int(s.nodes.size()); ++j)
                if (s.nodes[j].parent == i)
                    ch.append(j);
            if (!ch.isEmpty())
                no["children"] = ch;
            if (n.parent < 0)
                roots.append(i);
            nodes.append(no);
        }
        // Every primitive has its own mesh node; preserve the original mesh-node transform via parenting.
        for (const auto &m : s.meshes) {
            std::vector<float> pos, weights, normals, weights1, uv, uv1, tangents;
            std::vector<unsigned> joints, joints1;
            std::vector<QVector3D> ns(m.vertices.size());
            for (size_t i = 0; i < m.indices.size(); i += 3) {
                unsigned a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
                auto n = QVector3D::crossProduct(m.vertices[b].p - m.vertices[a].p,
                                                 m.vertices[c].p - m.vertices[a].p)
                             .normalized();
                ns[a] += n;
                ns[b] += n;
                ns[c] += n;
            }
            for (size_t i = 0; i < m.vertices.size(); ++i) {
                const auto &v = m.vertices[i];
                pos.insert(pos.end(), {v.p.x(), v.p.y(), v.p.z()});
                auto normal = v.normal.lengthSquared() > 1e-12f ? v.normal.normalized()
                              : ns[i].lengthSquared() > 1e-12f  ? ns[i].normalized()
                                                                : QVector3D(0, 1, 0);
                normals.insert(normals.end(), {normal.x(), normal.y(), normal.z()});
                tangents.insert(tangents.end(), {v.tangent.x(), v.tangent.y(), v.tangent.z(), v.tangent.w()});
                uv.insert(uv.end(), {v.uv.x(), v.uv.y()});
                uv1.insert(uv1.end(), {v.uv1.x(), v.uv1.y()});
                float sum = 0;
                for (float w : v.weights)
                    sum += w;
                for (int k = 0; k < 8; ++k) {
                    (k < 4 ? joints : joints1).push_back(unsigned(v.joints[k]));
                    (k < 4 ? weights : weights1).push_back(sum > 1e-6f ? v.weights[k] / sum : float(k == 0));
                }
            }
            QJsonObject attrs{{"POSITION", accessor(pos, 3, "VEC3", true)},
                              {"NORMAL", accessor(normals, 3, "VEC3")}};
            if (!m.joints.empty()) {
                attrs["JOINTS_0"] = unsignedAccessor(joints, 4, "VEC4", 5123);
                attrs["WEIGHTS_0"] = accessor(weights, 4, "VEC4");
                if (std::any_of(weights1.begin(), weights1.end(), [](float v) { return v > 0; })) {
                    attrs["JOINTS_1"] = unsignedAccessor(joints1, 4, "VEC4", 5123);
                    attrs["WEIGHTS_1"] = accessor(weights1, 4, "VEC4");
                }
            }
            if (std::any_of(m.vertices.begin(), m.vertices.end(),
                            [](const Vertex &v) { return v.tangent.lengthSquared() > 0; }))
                attrs["TANGENT"] = accessor(tangents, 4, "VEC4");
            if (!m.textures.isEmpty())
                attrs["TEXCOORD_0"] = accessor(uv, 2, "VEC2");
            bool hasUV1 = false;
            for (const auto &tex : m.textures)
                if (tex.coord == 1)
                    hasUV1 = true;
            if (hasUV1)
                attrs["TEXCOORD_1"] = accessor(uv1, 2, "VEC2");
            int mat = materials.size();
            QJsonObject pbr{{"baseColorFactor",
                             QJsonArray{m.color.redF(), m.color.greenF(), m.color.blueF(), m.color.alphaF()}},
                            {"metallicFactor", m.metallic},
                            {"roughnessFactor", m.roughness}};
            QJsonObject material{{"name", "Trickster surface"},
                                 {"doubleSided", m.doubleSided},
                                 {"alphaMode", m.alphaMode},
                                 {"alphaCutoff", m.alphaCutoff},
                                 {"emissiveFactor", vec(m.emissive)}};
            if (m.alphaMode != "MASK")
                material.remove("alphaCutoff");
            for (auto it = m.textures.cbegin(); it != m.textures.cend(); ++it) {
                const auto &t = it.value();
                while (bin.size() % 4)
                    bin.append('\0');
                int offset = int(bin.size());
                bin.append(t.bytes);
                int vi = views.size();
                views.append(
                    QJsonObject{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", t.bytes.size()}});
                int ii = images.size();
                images.append(QJsonObject{{"bufferView", vi}, {"mimeType", t.mime}});
                int si = textureSamplers.size();
                textureSamplers.append(QJsonObject{{"wrapS", t.wrapS},
                                                   {"wrapT", t.wrapT},
                                                   {"minFilter", t.minFilter},
                                                   {"magFilter", t.magFilter}});
                int ti = textures.size();
                textures.append(QJsonObject{{"source", ii}, {"sampler", si}});
                QJsonObject info{{"index", ti}, {"texCoord", t.coord}};
                if (t.offset != QVector2D{} || t.scale != QVector2D(1, 1) || t.rotation != 0) {
                    info["extensions"] =
                        QJsonObject{{"KHR_texture_transform",
                                     QJsonObject{{"offset", QJsonArray{t.offset.x(), t.offset.y()}},
                                                 {"scale", QJsonArray{t.scale.x(), t.scale.y()}},
                                                 {"rotation", t.rotation}}}};
                    auto used = root["extensionsUsed"].toArray();
                    if (!used.contains("KHR_texture_transform"))
                        used.append("KHR_texture_transform");
                    root["extensionsUsed"] = used;
                    root["extensionsRequired"] = QJsonArray{"KHR_texture_transform"};
                }
                if (it.key() == "normal")
                    info["scale"] = m.normalScale;
                if (it.key() == "occlusion")
                    info["strength"] = m.occlusionStrength;
                if (it.key() == "baseColor" || it.key() == "metallicRoughness")
                    pbr[it.key() + "Texture"] = info;
                else
                    material[it.key() + "Texture"] = info;
            }
            if (m.unlit) {
                material["extensions"] = QJsonObject{{"KHR_materials_unlit", QJsonObject{}}};
                auto used = root["extensionsUsed"].toArray();
                if (!used.contains("KHR_materials_unlit"))
                    used.append("KHR_materials_unlit");
                root["extensionsUsed"] = used;
            }
            material["pbrMetallicRoughness"] = pbr;
            materials.append(material);
            QJsonArray targets, targetNames;
            for (const auto &t : m.targets) {
                QJsonObject target;
                targetNames.append(t.name);
                for (const auto &prop : {QString("POSITION"), QString("NORMAL"), QString("TANGENT")}) {
                    const auto &delta = prop == "POSITION" ? t.positions
                                        : prop == "NORMAL" ? t.normals
                                                           : t.tangents;
                    if (!delta.empty()) {
                        std::vector<float> v;
                        for (auto d : delta)
                            v.insert(v.end(), {d.x(), d.y(), d.z()});
                        target[prop] = accessor(v, 3, "VEC3", prop == "POSITION");
                    }
                }
                targets.append(target);
            }
            int mi = meshes.size();
            QJsonObject primitive{{"attributes", attrs},
                                  {"indices", unsignedAccessor(m.indices, 1, "SCALAR", 5125)},
                                  {"material", mat},
                                  {"mode", 4}};
            QJsonObject meshObj;
            if (!targets.isEmpty()) {
                primitive["targets"] = targets;
                meshObj["weights"] = floats(s.nodes[m.node].morphRest);
                meshObj["extras"] = QJsonObject{{"targetNames", targetNames}};
            }
            meshObj["primitives"] = QJsonArray{primitive};
            meshes.append(meshObj);
            QJsonObject no{{"name", "Trickster mesh"}, {"mesh", mi}};
            if (!m.joints.empty()) {
                std::vector<float> ib;
                for (const auto &matrix : m.inverseBind)
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 4; ++r)
                            ib.push_back(matrix(r, c));
                QJsonArray js;
                for (int j : m.joints)
                    js.append(j);
                int si = skins.size();
                skins.append(QJsonObject{{"joints", js}, {"inverseBindMatrices", accessor(ib, 16, "MAT4")}});
                no["skin"] = si;
            }
            int ni = nodes.size();
            meshNodes.push_back(ni);
            nodes.append(no);
            if (m.node >= 0 && m.joints.empty()) {
                auto parent = nodes[m.node].toObject();
                auto ch = parent["children"].toArray();
                ch.append(ni);
                parent["children"] = ch;
                nodes[m.node] = parent;
            } else
                roots.append(ni);
        }
        auto stepChannels=steppedExportChannels(s);
        std::vector<Pose> baked;
        std::vector<MorphWeights> bakedMorphs;
        std::vector<float> times;
        const bool engineCompatible=s.physics.requests["trajectoryExport"].toObject()["mode"]=="engine-compatible";
        std::vector<double> exportTimes;
        if(engineCompatible||!s.curves.empty()||!stepChannels.empty())exportTimes=animationExportTimes(s);
        else for(int f=s.first;f<=s.last;f++)exportTimes.push_back(double(f)/s.fps);
        for(double time:exportTimes) {
            s.evaluate(float(time*s.fps));baked.push_back(s.pose);bakedMorphs.push_back(s.morphs);
            times.push_back(float(time-double(s.first)/s.fps));
        }
        if(!s.curves.empty()){auto asset=root["asset"].toObject();auto extras=asset["extras"].toObject();extras["tricksterCurveBake"]=QJsonObject{{"method","adaptive quarter-point transform/vertex sampling"},{"samples",int(exportTimes.size())},{"positionToleranceMetres",.0005},{"rotationToleranceDegrees",.1},{"scaleTolerance",.0001},{"morphTolerance",.0001},{"stepPolicy","Channels containing discontinuities use STEP samplers. Their continuous portions are explicitly refined to the sampled tolerance, avoiding consumer resampling ramps."},{"certificate","sampled, not a continuous mathematical bound"}};asset["extras"]=extras;root["asset"]=asset;}
        if(engineCompatible) {
            auto asset=root["asset"].toObject();auto extras=asset["extras"].toObject();
            extras["tricksterTrajectoryExport"]=QJsonObject{{"methodVersion","event-linear-export/1"},
                {"nominalFps",s.fps},{"samples",int(exportTimes.size())},{"interpolation","LINEAR"}};
            asset["extras"]=extras;root["asset"]=asset;
        }
        int input = accessor(times, 1, "SCALAR", true);
        QJsonArray samplers, channels;
        const AnimationClip *nativeClip = s.clips && s.sourceClip>=0 && s.sourceClip<int(s.clips->size())
            ? &(*s.clips)[s.sourceClip] : nullptr;
        const bool preserveChannels = s.curves.empty() && !engineCompatible && nativeClip && !nativeClip->channels.empty() && s.layers.empty() &&
            s.contacts.empty() && s.pins.empty() && s.jointLimits.empty() && s.rotationLimits.empty() &&
            s.first==0 && std::abs(double(s.last)/s.fps-nativeClip->duration)<1e-5;
        if(preserveChannels) {
            // glTF quaternion component tangents are derivatives per second, not angular velocities.
            for(const auto &ch:nativeClip->channels) {
                int channelInput=accessor(ch.times,1,"SCALAR",true);
                int components=ch.path=="weights"?1:ch.components;
                QString type=components==4?"VEC4":components==3?"VEC3":"SCALAR";
                int output=accessor(ch.values,components,type);
                auto add=[&](int target) {
                    int si=samplers.size();
                    samplers.append(QJsonObject{{"input",channelInput},{"output",output},{"interpolation",ch.interpolation}});
                    channels.append(QJsonObject{{"sampler",si},{"target",QJsonObject{{"node",target},{"path",ch.path}}}});
                };
                if(ch.path=="weights") {
                    for(size_t mi=0;mi<s.meshes.size();++mi)if(s.meshes[mi].node==ch.node&&!s.meshes[mi].targets.empty())add(meshNodes[mi]);
                } else add(ch.node);
            }
        } else {
        for (int ni = 0; ni < int(s.nodes.size()); ++ni)
            for (QString prop : {QString("translation"), QString("rotation"), QString("scale")}) {
                std::vector<float> values;
                QQuaternion previous;
                for (size_t f = 0; f < baked.size(); ++f) {
                    const auto &t = baked[f][ni];
                    if (prop == "rotation") {
                        auto q = t.r.normalized();
                        if (f && QQuaternion::dotProduct(previous, q) < 0)
                            q = -q;
                        values.insert(values.end(), {q.x(), q.y(), q.z(), q.scalar()});
                        previous = q;
                    } else {
                        auto v = prop == "translation" ? t.t : t.s;
                        values.insert(values.end(), {v.x(), v.y(), v.z()});
                    }
                }
                int output =
                    accessor(values, prop == "rotation" ? 4 : 3, prop == "rotation" ? "VEC4" : "VEC3");
                int si = samplers.size();
                samplers.append(
                    QJsonObject{{"input", input}, {"output", output}, {"interpolation", stepChannels.contains(QString::number(ni)+":"+prop) ? "STEP" : "LINEAR"}});
                channels.append(
                    QJsonObject{{"sampler", si}, {"target", QJsonObject{{"node", ni}, {"path", prop}}}});
            }
        for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
            const auto &m = s.meshes[mi];
            if (m.targets.empty())
                continue;
            std::vector<float> values;
            for (const auto &w : bakedMorphs) {
                auto v = w.value(m.node, s.nodes[m.node].morphRest);
                values.insert(values.end(), v.begin(), v.end());
            }
            int output = accessor(values, 1, "SCALAR"), si = samplers.size();
            samplers.append(QJsonObject{{"input", input}, {"output", output}, {"interpolation", stepChannels.contains(QString::number(s.meshes[mi].node)+":weights")?"STEP":"LINEAR"}});
            channels.append(QJsonObject{
                {"sampler", si}, {"target", QJsonObject{{"node", meshNodes[mi]}, {"path", "weights"}}}});
        }
        }
        if (!images.isEmpty()) {
            root["images"] = images;
            root["textures"] = textures;
            root["samplers"] = textureSamplers;
        }
        root["nodes"] = nodes;
        root["meshes"] = meshes;
        if (!skins.isEmpty())
            root["skins"] = skins;
        root["materials"] = materials;
        root["scenes"] = QJsonArray{QJsonObject{{"name", s.name}, {"nodes", roots}}};
        root["scene"] = 0;
        root["accessors"] = accessors;
        for (const auto &mv : meshes)
            for (const auto &pv : mv.toObject()["primitives"].toArray()) {
                auto prim = pv.toObject();
                auto attributes = prim["attributes"].toObject();
                for (auto it = attributes.constBegin(); it != attributes.constEnd(); ++it) {
                    int vi = accessors[it.value().toInt()].toObject()["bufferView"].toInt();
                    auto view = views[vi].toObject();
                    view["target"] = 34962;
                    views[vi] = view;
                }
                for (auto tv : prim["targets"].toArray()) {
                    auto target = tv.toObject();
                    for (auto it = target.constBegin(); it != target.constEnd(); ++it) {
                        int vi = accessors[it.value().toInt()].toObject()["bufferView"].toInt();
                        auto view = views[vi].toObject();
                        view["target"] = 34962;
                        views[vi] = view;
                    }
                }
                int vi = accessors[prim["indices"].toInt()].toObject()["bufferView"].toInt();
                auto view = views[vi].toObject();
                view["target"] = 34963;
                views[vi] = view;
            }
        root["bufferViews"] = views;
        root["animations"] =
            QJsonArray{QJsonObject{{"name", s.name}, {"samplers", samplers}, {"channels", channels}}};
        bool glb = QFileInfo(path).suffix().compare("glb", Qt::CaseInsensitive) == 0;
        QJsonObject buffer{{"byteLength", int(bin.size())}};
        if (!glb)
            buffer["uri"] = "data:application/octet-stream;base64," + QString::fromLatin1(bin.toBase64());
        root["buffers"] = QJsonArray{buffer};
        QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
        if (!glb)
            return writeFile(path, json, error);
        while (json.size() % 4)
            json.append(' ');
        while (bin.size() % 4)
            bin.append('\0');
        QByteArray out;
        appendU32(out, 0x46546c67);
        appendU32(out, 2);
        appendU32(out, quint32(12 + 8 + json.size() + 8 + bin.size()));
        appendU32(out, quint32(json.size()));
        appendU32(out, 0x4e4f534a);
        out.append(json);
        appendU32(out, quint32(bin.size()));
        appendU32(out, 0x004e4942);
        out.append(bin);
        return writeFile(path, out, error);
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
} // namespace trickster
