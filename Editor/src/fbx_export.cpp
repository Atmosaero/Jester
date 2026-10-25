#include "runtime_error.h"
#include "core.h"
#include "ufbx.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <QtMath>
#include <cmath>
#include <stdexcept>

namespace trickster {
namespace {
QString quote(QString s) {
    auto b = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(b.mid(1, b.size() - 2));
}
QVector3D euler(QQuaternion q) {
    auto v = ufbx_quat_to_euler({q.x(), q.y(), q.z(), q.scalar()}, UFBX_ROTATION_ORDER_XYZ);
    return {float(v.x), float(v.y), float(v.z)};
}
struct Writer {
    QString objects, connections;
    QTextStream out{&objects}, conn{&connections};
    qint64 next = 100000;
    Writer() {
        out.setRealNumberPrecision(9);
    }
    qint64 id() {
        return ++next;
    }
    void link(qint64 child, qint64 parent, QString property = {}) {
        conn << " C: " << (property.isEmpty() ? "\"OO\"" : "\"OP\"") << ", " << child << ", " << parent;
        if (!property.isEmpty())
            conn << ", " << quote(property);
        conn << "\n";
    }
    template <class T> void array(QString name, const std::vector<T> &values) {
        out << name << ": *" << values.size() << " { a: ";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i)
                out << ",";
            out << values[i];
        }
        out << " }\n";
    }
    void vectorProperty(QString name, QVector3D v) {
        out << " P: " << quote(name) << ", \"" << name << "\", \"\", \"A\", " << v.x() << "," << v.y() << ","
            << v.z() << "\n";
    }
    void matrix(QString name, QMatrix4x4 m) {
        std::vector<float> v;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                v.push_back(m(r, c));
        array(name, v);
    }
    qint64 model(QString name, QString type, const Transform &t) {
        auto model = id();
        out << "Model: " << model << ", " << quote("Model::" + name) << ", " << quote(type)
            << " {\n Version: 232\n Properties70: {\n";
        vectorProperty("Lcl Translation", t.t);
        vectorProperty("Lcl Rotation", euler(t.r));
        vectorProperty("Lcl Scaling", t.s);
        out << " P: \"RotationOrder\", \"enum\", \"\", \"\", 0\n P: \"InheritType\", \"enum\", \"\", \"\", "
               "1\n }\n Shading: T\n Culling: \"CullingOff\"\n}\n";
        return model;
    }
    void curves(qint64 owner, QString property, const std::vector<QVector3D> &v,
                const std::vector<qint64> &times, qint64 layer, bool scalar = false, bool stepped = false) {
        auto node = id();
        out << "AnimationCurveNode: " << node << ", " << quote("AnimCurveNode::" + property)
            << ", \"\" { Properties70: {\n";
        for (int axis = 0; axis < (scalar ? 1 : 3); ++axis)
            out << " P: " << quote(scalar ? "d|DeformPercent" : QString("d|%1").arg(QChar("XYZ"[axis])))
                << ", \"Number\", \"\", \"A\", " << v[0][axis] << "\n";
        out << "} }\n";
        link(node, layer);
        link(node, owner, property);
        for (int axis = 0; axis < (scalar ? 1 : 3); ++axis) {
            auto curve = id();
            out << "AnimationCurve: " << curve << ", \"AnimCurve::\", \"\" {\n Default: " << v[0][axis]
                << "\n KeyVer: 4008\n";
            array("KeyTime", times);
            std::vector<float> values;
            for (auto value : v)
                values.push_back(value[axis]);
            array("KeyValueFloat", values);
            array("KeyAttrFlags", std::vector<int>{stepped ? 2 : 4});
            array("KeyAttrDataFloat", std::vector<float>{0, 0, 0, 0});
            array("KeyAttrRefCount", std::vector<int>{int(times.size())});
            out << "}\n";
            link(curve, node, scalar ? "d|DeformPercent" : QString("d|%1").arg(QChar("XYZ"[axis])));
        }
    }
};
} // namespace
bool exportFbx(const QString &path, const Scene &original, QString &error) {
    try {
        auto need = [](bool ok, QString text) {
            if (!ok)
                throw trickster::RuntimeError(text.toStdString());
        };
        need(!original.nodes.empty(), "Empty scene: add a character before export.");
        need(original.validate(&error), error);
        need(qint64(original.last - original.first + 1) * qint64(original.nodes.size()) <= 5000000,
             "Animation is too large to bake.");
        Scene s = original;
        for (auto &mesh : s.meshes)
            completeGeometryBasis(mesh);
        auto stepChannels=steppedExportChannels(s);
        std::vector<Pose> poses;
        std::vector<MorphWeights> morphs;
        std::vector<qint64> times;
        for(double t:animationExportTimes(s)) {
            s.evaluate(float(t*s.fps));poses.push_back(s.pose);morphs.push_back(s.morphs);
            times.push_back(qint64(std::llround((t-double(s.first)/s.fps)*46186158000.)));
        }
        Writer w;
        std::vector<qint64> models;
        for (size_t i = 0; i < s.nodes.size(); ++i)
            models.push_back(
                w.model(s.nodes[i].name, s.nodes[i].joint ? "LimbNode" : "Null", s.nodes[i].rest));
        for (size_t i = 0; i < s.nodes.size(); ++i)
            w.link(models[i], s.nodes[i].parent < 0 ? 0 : models[s.nodes[i].parent]);
        auto stack = w.id(), layer = w.id();
        w.out << "AnimationStack: " << stack << ", " << quote("AnimStack::" + s.name)
              << ", \"\" { Properties70: {\n P: \"LocalStart\", \"KTime\", \"Time\", \"\", 0\n P: "
                 "\"LocalStop\", \"KTime\", \"Time\", \"\", "
              << times.back() << "\n } }\n";
        w.out << "AnimationLayer: " << layer << ", \"AnimLayer::Base\", \"\" { }\n";
        w.link(layer, stack);
        for (size_t n = 0; n < s.nodes.size(); ++n)
            for (QString property :
                 {QString("Lcl Translation"), QString("Lcl Rotation"), QString("Lcl Scaling")}) {
                std::vector<QVector3D> values;
                QVector3D previous;
                for (const auto &p : poses) {
                    auto v = property == "Lcl Translation" ? p[n].t
                             : property == "Lcl Scaling"   ? p[n].s
                                                           : euler(p[n].r);
                    if (property == "Lcl Rotation" && !values.empty())
                        for (int j = 0; j < 3; ++j) {
                            while (v[j] - previous[j] > 180)
                                v[j] -= 360;
                            while (v[j] - previous[j] < -180)
                                v[j] += 360;
                        }
                    values.push_back(v);
                    previous = v;
                }
                w.curves(models[n], property, values, times, layer, false,
                    stepChannels.contains(QString::number(n)+":"+(property=="Lcl Translation"?"translation":property=="Lcl Rotation"?"rotation":"scale")));
            }
        for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
            const auto &m = s.meshes[mi];
            auto geom = w.id(), model = w.model(QString("Surface_%1").arg(mi), "Mesh", Transform{});
            w.link(model, m.joints.empty() && m.node >= 0 ? models[m.node] : 0);
            w.link(geom, model);
            w.out << "Geometry: " << geom << ", " << quote(QString("Geometry::Surface_%1").arg(mi))
                  << ", \"Mesh\" {\n";
            std::vector<float> vertices, normals, uv, uv1;
            std::vector<int> polygon;
            for (const auto &v : m.vertices) {
                vertices.insert(vertices.end(), {v.p.x(), v.p.y(), v.p.z()});
                normals.insert(normals.end(), {v.normal.x(), v.normal.y(), v.normal.z()});
                uv.insert(uv.end(), {v.uv.x(), 1 - v.uv.y()});
                uv1.insert(uv1.end(), {v.uv1.x(), 1 - v.uv1.y()});
            }
            for (size_t i = 0; i < m.indices.size(); ++i)
                polygon.push_back(i % 3 == 2 ? -int(m.indices[i]) - 1 : int(m.indices[i]));
            w.array("Vertices", vertices);
            w.array("PolygonVertexIndex", polygon);
            w.out << "LayerElementNormal: 0 { Version: 101\n Name: \"\"\n MappingInformationType: "
                     "\"ByVertice\"\n ReferenceInformationType: \"Direct\"\n";
            w.array("Normals", normals);
            w.out << "}\n";
            for (int set = 0; set < 2; ++set) {
                w.out << "LayerElementUV: " << set
                      << " { Version: 101\n Name: " << quote(QString("UV%1").arg(set))
                      << "\n MappingInformationType: \"ByVertice\"\n ReferenceInformationType: \"Direct\"\n";
                w.array("UV", set ? uv1 : uv);
                w.out << "}\n";
            }
            w.out
                << "LayerElementMaterial: 0 { Version: 101\n Name: \"\"\n MappingInformationType: "
                   "\"AllSame\"\n ReferenceInformationType: \"IndexToDirect\"\n Materials: *1 { a: 0 }\n}\n";
            for (int set = 0; set < 2; ++set)
                w.out << "Layer: " << set
                      << " { Version: 100\n LayerElement: { Type: \"LayerElementUV\"\n TypedIndex: " << set
                      << " }\n"
                      << (set == 0 ? " LayerElement: { Type: \"LayerElementNormal\"\n TypedIndex: 0 }\n "
                                     "LayerElement: { Type: \"LayerElementMaterial\"\n TypedIndex: 0 }\n"
                                   : "")
                      << "}\n";
            w.out << "}\n";
            auto mat = w.id();
            w.link(mat, model);
            w.out << "Material: " << mat << R"FBX(, "Material::Surface", "" { Version: 102
 ShadingModel: "phong"
 MultiLayer: 0
 Properties70: {
 P: "DiffuseColor", "Color", "", "A", )FBX"
                  << m.color.redF() << "," << m.color.greenF() << "," << m.color.blueF() << R"FBX(
 P: "EmissiveColor", "Color", "", "A", )FBX"
                  << m.emissive.x() << "," << m.emissive.y() << "," << m.emissive.z() << "\n";
            auto number = [&](QString name, float value) {
                w.out << " P: " << quote(name) << R"FBX(, "Number", "", "AU", )FBX" << value << "\n";
            };
            number("TransparencyFactor", 1 - m.color.alphaF());
            number("TricksterMetallic", m.metallic);
            number("TricksterRoughness", m.roughness);
            number("TricksterNormalScale", m.normalScale);
            number("TricksterOcclusionStrength", m.occlusionStrength);
            number("TricksterAlphaCutoff", m.alphaCutoff);
            number("TricksterUnlit", float(m.unlit));
            number("TricksterDoubleSided", float(m.doubleSided));
            w.out << R"FBX( P: "TricksterAlphaMode", "KString", "", "U", )FBX" << quote(m.alphaMode)
                  << "\n } }\n";
            for (auto it = m.textures.cbegin(); it != m.textures.cend(); ++it) {
                const auto &t = it.value();
                QString extension = t.mime == "image/jpeg" ? "jpg" : "png",
                        hash = QString::fromLatin1(
                            QCryptographicHash::hash(t.bytes, QCryptographicHash::Sha256).toHex().left(20));
                QString folder = QFileInfo(path).completeBaseName() + ".fbm",
                        relative = folder + "/" + hash + "." + extension;
                auto dir = QFileInfo(path).dir();
                need(dir.mkpath(folder), "Cannot create texture folder.");
                QSaveFile image(dir.filePath(relative));
                need(image.open(QIODevice::WriteOnly) && image.write(t.bytes) == t.bytes.size() &&
                         image.commit(),
                     "Cannot export texture: " + relative);
                QMatrix4x4 flip;
                flip.translate(0, 1, 0);
                flip.scale(1, -1, 1);
                QMatrix4x4 samplingFrame;
                samplingFrame.translate(t.offset.x(), t.offset.y());
                samplingFrame.rotate(qRadiansToDegrees(t.rotation), 0, 0, 1);
                samplingFrame.scale(t.scale.x(), t.scale.y(), 1);
                bool invertible = false;
                auto frame = flip * samplingFrame.inverted(&invertible) * flip;
                auto x = frame.column(0).toVector3D(), y = frame.column(1).toVector3D();
                need(invertible && x.lengthSquared() > 1e-12f && y.lengthSquared() > 1e-12f &&
                         std::abs(QVector3D::dotProduct(x.normalized(), y.normalized())) < .0001f,
                     "This UV transform needs baking before FBX export. GLB preserves it directly.");
                auto video = w.id(), tex = w.id();
                w.out << "Video: " << video
                      << ", \"Video::Image\", \"Clip\" { Type: \"Clip\"\n Filename: " << quote(relative)
                      << "\n RelativeFilename: " << quote(relative) << "\n }\n";
                w.out << "Texture: " << tex
                      << ", \"Texture::Image\", \"\" { Type: \"TextureVideoClip\"\n Version: 202\n "
                         "TextureName: \"Texture::Image\"\n FileName: "
                      << quote(relative) << "\n RelativeFilename: " << quote(relative)
                      << "\n Properties70: { P: \"UVSet\", \"KString\", \"\", \"\", "
                      << quote(QString("UV%1").arg(t.coord)) << "\n P: \"WrapModeU\", \"enum\", \"\", \"\", "
                      << (t.wrapS == 33071 ? 1 : 0) << "\n P: \"WrapModeV\", \"enum\", \"\", \"\", "
                      << (t.wrapT == 33071 ? 1 : 0) << "\n";
                w.out << R"FBX( P: "Translation", "Vector3D", "Vector", "", )FBX" << frame(0, 3) << ","
                      << frame(1, 3) << R"FBX(,0
 P: "Rotation", "Vector3D", "Vector", "", 0,0,)FBX"
                      << qRadiansToDegrees(std::atan2(frame(1, 0), frame(0, 0))) << R"FBX(
 P: "Scaling", "Vector3D", "Vector", "", )FBX"
                      << x.length() << ","
                      << (frame(0, 0) * frame(1, 1) - frame(1, 0) * frame(0, 1)) / x.length() << ",1\n";
                number("TricksterWrapS", float(t.wrapS));
                number("TricksterWrapT", float(t.wrapT));
                number("TricksterMinFilter", float(t.minFilter));
                number("TricksterMagFilter", float(t.magFilter));
                w.out << " } }\n";
                w.link(video, tex);
                w.link(tex, mat,
                       it.key() == "baseColor"   ? "DiffuseColor"
                       : it.key() == "normal"    ? "NormalMap"
                       : it.key() == "emissive"  ? "EmissiveColor"
                       : it.key() == "occlusion" ? "TricksterOcclusionMap"
                                                 : "TricksterMetallicRoughness");
            }
            if (!m.joints.empty()) {
                auto skin = w.id();
                w.out << "Deformer: " << skin
                      << ", \"Deformer::Skin\", \"Skin\" { Version: 101\n Link_DeformAcuracy: 50\n "
                         "SkinningType: \"Linear\"\n }\n";
                w.link(skin, geom);
                for (size_t j = 0; j < m.joints.size(); ++j) {
                    std::vector<int> indices;
                    std::vector<float> weights;
                    for (size_t vi = 0; vi < m.vertices.size(); ++vi) {
                        const auto &v = m.vertices[vi];
                        float sum = 0, value = 0;
                        for (int k = 0; k < 8; ++k) {
                            sum += v.weights[k];
                            if (v.joints[k] == int(j))
                                value += v.weights[k];
                        }
                        if (value > 0 && sum > 0) {
                            indices.push_back(int(vi));
                            weights.push_back(value / sum);
                        }
                    }
                    auto cluster = w.id();
                    w.out << "Deformer: " << cluster
                          << ", \"SubDeformer::Cluster\", \"Cluster\" { Version: 100\n";
                    w.array("Indexes", indices);
                    w.array("Weights", weights);
                    w.matrix("Transform", m.inverseBind[j]);
                    w.matrix("TransformLink", m.inverseBind[j].inverted());
                    w.out << "}\n";
                    w.link(cluster, skin);
                    w.link(models[m.joints[j]], cluster);
                }
            }
            if (!m.targets.empty()) {
                auto blend = w.id();
                w.out << "Deformer: " << blend << ", \"Deformer::Blend\", \"BlendShape\" { Version: 100 }\n";
                w.link(blend, geom);
                for (size_t j = 0; j < m.targets.size(); ++j) {
                    const auto &target = m.targets[j];
                    auto ch = w.id(), shape = w.id();
                    w.out << "Deformer: " << ch << ", " << quote("SubDeformer::" + target.name)
                          << ", \"BlendShapeChannel\" { Version: 100\n DeformPercent: "
                          << s.nodes[m.node].morphRest[j] * 100 << "\n FullWeights: *1 { a: 100 }\n }\n";
                    w.link(ch, blend);
                    w.link(shape, ch);
                    w.out << "Geometry: " << shape << ", " << quote("Geometry::" + target.name)
                          << ", \"Shape\" { Version: 100\n";
                    std::vector<int> ix;
                    std::vector<float> pos, nor;
                    for (size_t vi = 0; vi < m.vertices.size(); ++vi) {
                        ix.push_back(int(vi));
                        auto p = target.positions.empty() ? QVector3D{} : target.positions[vi],
                             n = target.normals.empty() ? QVector3D{} : target.normals[vi];
                        pos.insert(pos.end(), {p.x(), p.y(), p.z()});
                        nor.insert(nor.end(), {n.x(), n.y(), n.z()});
                    }
                    w.array("Indexes", ix);
                    w.array("Vertices", pos);
                    w.array("Normals", nor);
                    w.out << "}\n";
                    std::vector<QVector3D> values;
                    for (const auto &weights : morphs)
                        values.push_back({weights.value(m.node, s.nodes[m.node].morphRest)[j] * 100, 0, 0});
                    w.curves(ch, "DeformPercent", values, times, layer, true,stepChannels.contains(QString::number(m.node)+":weights"));
                }
            }
        }
        QString text;
        QTextStream out(&text);
        out << "; FBX 7.4.0 project file\n; Generated by Trickster\nFBXHeaderExtension: { FBXHeaderVersion: "
               "1003\n FBXVersion: 7400\n Creator: \"Trickster\"\n}\nGlobalSettings: { Version: 1000\n "
               "Properties70: {\n P: \"UpAxis\", \"int\", \"Integer\", \"\", 1\n P: \"UpAxisSign\", \"int\", "
               "\"Integer\", \"\", 1\n P: \"FrontAxis\", \"int\", \"Integer\", \"\", 2\n P: "
               "\"FrontAxisSign\", \"int\", \"Integer\", \"\", 1\n P: \"CoordAxis\", \"int\", \"Integer\", "
               "\"\", 0\n P: \"CoordAxisSign\", \"int\", \"Integer\", \"\", 1\n P: \"UnitScaleFactor\", "
               "\"double\", \"Number\", \"\", 100\n P: \"TimeMode\", \"enum\", \"\", \"\", 14\n P: "
               "\"CustomFrameRate\", \"double\", \"Number\", \"\", "
            << s.fps
            << "\n } }\nDocuments: { Count: 1\n Document: 1, \"Scene\", \"Scene\" { RootNode: 0 "
               "}\n}\nDefinitions: { Version: 100\n Count: "
            << w.next - 100000 << "\n}\nObjects: {\n"
            << w.objects << "}\nConnections: {\n"
            << w.connections << "}\nTakes: { Current: " << quote(s.name) << " }\n";
        QSaveFile f(path);
        auto data = text.toUtf8();
        need(f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit(),
             "Cannot write FBX: " + f.errorString());
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
} // namespace trickster
