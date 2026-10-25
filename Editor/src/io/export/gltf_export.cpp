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

#include "io/project/json_values.h"
namespace trickster {
using namespace project_detail;
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
