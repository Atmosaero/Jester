#include "io/project/json_values.h"
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
using namespace project_detail;
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

} // namespace trickster
