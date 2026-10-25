#include "core/scene/runtime_error.h"
#include "core/scene/core.h"
#include "ufbx.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace trickster {
namespace {
QString str(ufbx_string s) {
    return QString::fromUtf8(s.data, qsizetype(s.length));
}
QVector3D vec(ufbx_vec3 v) {
    return {float(v.x), float(v.y), float(v.z)};
}
Transform transform(ufbx_transform t) {
    return {vec(t.translation),
            QQuaternion(float(t.rotation.w), float(t.rotation.x), float(t.rotation.y), float(t.rotation.z))
                .normalized(),
            vec(t.scale)};
}
QMatrix4x4 matrix(ufbx_matrix m) {
    return QMatrix4x4(float(m.m00), float(m.m01), float(m.m02), float(m.m03), float(m.m10), float(m.m11),
                      float(m.m12), float(m.m13), float(m.m20), float(m.m21), float(m.m22), float(m.m23), 0,
                      0, 0, 1);
}
void require(bool ok, const QString &message) {
    if (!ok)
        throw trickster::RuntimeError(message.toStdString());
}
using Fbx = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;
Texture texture(ufbx_texture *t, const QString &path, const ufbx_mesh *mesh) {
    require(t && t->file_textures.count > 0, "FBX procedural texture must be baked to an image.");
    t = t->file_textures.data[0];
    Texture out;
    QMatrix4x4 flip;
    flip.translate(0, 1, 0);
    flip.scale(1, -1, 1);
    auto uv = flip * matrix(t->uv_to_texture) * flip;
    auto x = uv.column(0).toVector3D(), y = uv.column(1).toVector3D();
    require(x.lengthSquared() > 1e-12f && y.lengthSquared() > 1e-12f &&
                std::abs(QVector3D::dotProduct(x.normalized(), y.normalized())) < .0001f,
            "Bake sheared FBX UV transforms before import.");
    out.offset = {uv(0, 3), uv(1, 3)};
    out.scale = {x.length(), (uv(0, 0) * uv(1, 1) - uv(1, 0) * uv(0, 1)) / x.length()};
    out.rotation = std::atan2(uv(1, 0), uv(0, 0));
    for (size_t i = 0; i < mesh->uv_sets.count; ++i)
        if (str(mesh->uv_sets.data[i].name) == str(t->uv_set)) {
            require(i < 2, "Only two UV sets are supported.");
            out.coord = int(i);
        }
    require(t->content.size <= 64 * 1024 * 1024, "Embedded FBX texture exceeds 64 MB.");
    if (t->content.size)
        out.bytes = QByteArray(static_cast<const char *>(t->content.data), qsizetype(t->content.size));
    else {
        QStringList candidates{
            str(t->filename), QFileInfo(path).dir().filePath(str(t->relative_filename)),
            QFileInfo(path).dir().filePath(QFileInfo(str(t->absolute_filename)).fileName())};
        for (const auto &candidate : candidates) {
            QFile f(candidate);
            if (f.open(QIODevice::ReadOnly) && f.size() <= 64 * 1024 * 1024) {
                out.bytes = f.readAll();
                break;
            }
        }
        require(!out.bytes.isEmpty(), "Missing FBX texture: " + str(t->relative_filename));
    }
    QBuffer b(&out.bytes);
    b.open(QIODevice::ReadOnly);
    QImageReader reader(&b);
    auto size = reader.size();
    require(size.isValid() && qint64(size.width()) * size.height() <= 64000000,
            "Unsupported or oversized FBX texture.");
    // Normalize image formats for portable glTF export.
    auto format = reader.format();
    if (format == "jpeg" || format == "jpg")
        out.mime = "image/jpeg";
    else if (format != "png") {
        auto image = reader.read();
        require(!image.isNull(), "Cannot decode FBX image.");
        out.bytes.clear();
        QBuffer encoded(&out.bytes);
        encoded.open(QIODevice::WriteOnly);
        require(image.save(&encoded, "PNG"), "Cannot encode image.");
    }
    out.wrapS = t->wrap_u == UFBX_WRAP_CLAMP ? 33071 : 10497;
    out.wrapT = t->wrap_v == UFBX_WRAP_CLAMP ? 33071 : 10497;
    out.wrapS = int(ufbx_find_real(&t->props, "TricksterWrapS", out.wrapS));
    out.wrapT = int(ufbx_find_real(&t->props, "TricksterWrapT", out.wrapT));
    out.minFilter = int(ufbx_find_real(&t->props, "TricksterMinFilter", out.minFilter));
    out.magFilter = int(ufbx_find_real(&t->props, "TricksterMagFilter", out.magFilter));
    return out;
}
} // namespace
bool loadFbx(const QString &path, Scene &scene, QString &error) {
    try {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), file.errorString());
        require(file.size() <= 512 * 1024 * 1024, "FBX exceeds 512 MB.");
        auto bytes = file.readAll();
        auto filename = QFileInfo(path).absoluteFilePath().toUtf8();
        ufbx_load_opts opts{};
        opts.filename = {filename.constData(), size_t(filename.size())};
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1;
        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
        opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
        opts.generate_missing_normals = true;
        opts.clean_skin_weights = true;
        opts.use_blender_pbr_material = true;
        opts.temp_allocator.memory_limit = 1024ull * 1024 * 1024;
        opts.result_allocator.memory_limit = 1024ull * 1024 * 1024;
        ufbx_error e{};
        Fbx fbx(ufbx_load_memory(bytes.constData(), size_t(bytes.size()), &opts, &e), ufbx_free_scene);
        if (!fbx) {
            char message[4096];
            ufbx_format_error(message, sizeof(message), &e);
            require(false, QString::fromUtf8(message));
        }
        require(fbx->nodes.count <= 4096, "FBX exceeds 4096 nodes.");
        Scene s;
        s.name = QFileInfo(path).completeBaseName();
        s.fps = std::clamp(int(std::round(fbx->settings.frames_per_second)), 1, 240);
        for (auto n : fbx->nodes)
            s.nodes.push_back({str(n->name),
                               n->parent ? int(n->parent->typed_id) : -1,
                               transform(n->local_transform),
                               n->bone != nullptr,
                               {}});
        s.pose = s.restPose();
        // Expand indexed polygon corners so UV and hard-normal seams remain exact.
        for (auto node : fbx->nodes) {
            auto mesh = node->mesh;
            if (!mesh)
                continue;
            auto skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
            if (skin && skin->skinning_method != UFBX_SKINNING_METHOD_LINEAR)
                s.warnings << "FBX uses dual-quaternion skinning; converted to linear skinning.";
            struct TargetSource {
                ufbx_blend_channel *channel;
                size_t key;
            };
            std::vector<TargetSource> channels;
            for (auto deformer : mesh->blend_deformers)
                for (auto ch : deformer->channels) {
                    for (size_t k = 0; k < ch->keyframes.count; ++k) {
                        channels.push_back({ch, k});
                        s.nodes[node->typed_id].morphRest.push_back(
                            float(ch->keyframes.data[k].effective_weight));
                    }
                }
            for (size_t part = 0; part < std::max(size_t(1), mesh->materials.count); ++part) {
                Mesh m;
                m.node = int(node->typed_id);
                m.color = Qt::white;
                if (skin)
                    for (auto c : skin->clusters) {
                        require(c->bone_node != nullptr, "Missing skin bone.");
                        m.joints.push_back(int(c->bone_node->typed_id));
                        m.inverseBind.push_back(matrix(c->geometry_to_bone));
                        s.nodes[c->bone_node->typed_id].joint = true;
                    }
                std::vector<std::vector<QVector3D>> shapePos, shapeNormal;
                for (auto source : channels) {
                    auto ch = source.channel;
                    auto shape = ch->keyframes.data[source.key].shape;
                    std::vector<QVector3D> p(mesh->num_vertices), n(mesh->num_vertices);
                    if (shape)
                        for (size_t k = 0; k < shape->num_offsets; ++k) {
                            auto ix = shape->offset_vertices.data[k];
                            if (ix < p.size()) {
                                p[ix] = vec(shape->position_offsets.data[k]);
                                if (k < shape->normal_offsets.count)
                                    n[ix] = vec(shape->normal_offsets.data[k]);
                            }
                        }
                    shapePos.push_back(std::move(p));
                    shapeNormal.push_back(std::move(n));
                    m.targets.push_back(
                        {str(ch->name) +
                             (ch->keyframes.count > 1 ? QString("_%1").arg(source.key + 1) : QString{}),
                         {},
                         {}});
                }
                auto material = part < node->materials.count ? node->materials.data[part] : nullptr;
                if (material) {
                    const auto &color = material->pbr.base_color.has_value ? material->pbr.base_color
                                                                           : material->fbx.diffuse_color;
                    auto c = color.value_vec3;
                    float opacity =
                        material->pbr.opacity.has_value ? float(material->pbr.opacity.value_real) : 1;
                    m.color = QColor::fromRgbF(std::clamp(c.x, 0., 1.), std::clamp(c.y, 0., 1.),
                                               std::clamp(c.z, 0., 1.), std::clamp(opacity, 0.f, 1.f));
                    m.roughness = float(material->pbr.roughness.value_real);
                    m.metallic = float(material->pbr.metalness.value_real);
                    m.emissive = vec(material->pbr.emission_color.value_vec3);
                    if (opacity < 1)
                        m.alphaMode = "BLEND";
                    auto base = material->pbr.base_color.texture ? material->pbr.base_color.texture
                                                                 : material->fbx.diffuse_color.texture;
                    if (base)
                        m.textures["baseColor"] = texture(base, path, mesh);
                    auto normal = material->pbr.normal_map.texture ? material->pbr.normal_map.texture
                                                                   : material->fbx.normal_map.texture;
                    if (normal)
                        m.textures["normal"] = texture(normal, path, mesh);
                    if (material->pbr.emission_color.texture)
                        m.textures["emissive"] = texture(material->pbr.emission_color.texture, path, mesh);
                    if (material->pbr.ambient_occlusion.texture)
                        m.textures["occlusion"] =
                            texture(material->pbr.ambient_occlusion.texture, path, mesh);
                    m.metallic = float(ufbx_find_real(&material->props, "TricksterMetallic", m.metallic));
                    m.roughness = float(ufbx_find_real(&material->props, "TricksterRoughness", m.roughness));
                    m.normalScale = float(ufbx_find_real(&material->props, "TricksterNormalScale", 1));
                    m.occlusionStrength =
                        float(ufbx_find_real(&material->props, "TricksterOcclusionStrength", 1));
                    m.alphaCutoff = float(ufbx_find_real(&material->props, "TricksterAlphaCutoff", .5));
                    m.unlit = ufbx_find_real(&material->props, "TricksterUnlit", 0) != 0;
                    m.doubleSided = ufbx_find_real(&material->props, "TricksterDoubleSided", 1) != 0;
                    if (auto prop = ufbx_find_prop(&material->props, "TricksterAlphaMode"))
                        m.alphaMode = str(prop->value_str);
                    for (auto mt : material->textures) {
                        auto name = str(mt.material_prop);
                        if (name == "TricksterMetallicRoughness")
                            m.textures["metallicRoughness"] = texture(mt.texture, path, mesh);
                        if (name == "TricksterOcclusionMap")
                            m.textures["occlusion"] = texture(mt.texture, path, mesh);
                    }
                }
                for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
                    if (mesh->face_material.count && mesh->face_material.data[fi] != part)
                        continue;
                    auto face = mesh->faces.data[fi];
                    if (face.num_indices < 3)
                        continue;
                    require(m.vertices.size() + face.num_indices * 3 <= 2000000,
                            "FBX mesh exceeds vertex limit.");
                    std::vector<uint32_t> triangles(size_t(face.num_indices - 2) * 3);
                    auto count = ufbx_triangulate_face(triangles.data(), triangles.size(), mesh, face);
                    for (size_t k = 0; k < size_t(count) * 3; ++k) {
                        uint32_t corner = triangles[k], vi = mesh->vertex_indices.data[corner];
                        Vertex v;
                        v.p = vec(ufbx_get_vertex_vec3(&mesh->vertex_position, corner));
                        v.normal = vec(ufbx_get_vertex_vec3(&mesh->vertex_normal, corner));
                        if (mesh->vertex_uv.exists) {
                            auto uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, corner);
                            v.uv = {float(uv.x), 1 - float(uv.y)};
                        }
                        if (mesh->uv_sets.count > 1) {
                            auto uv = ufbx_get_vertex_vec2(&mesh->uv_sets.data[1].vertex_uv, corner);
                            v.uv1 = {float(uv.x), 1 - float(uv.y)};
                        }
                        if (skin) {
                            v.weights.fill(0);
                            auto influences = skin->vertices.data[vi];
                            if (influences.num_weights > 8)
                                s.warnings
                                    << "FBX vertices with more than eight influences were reduced to eight.";
                            for (uint32_t j = 0; j < std::min(8u, influences.num_weights); ++j) {
                                auto w = skin->weights.data[influences.weight_begin + j];
                                v.joints[j] = int(w.cluster_index);
                                v.weights[j] = float(w.weight);
                            }
                        }
                        for (size_t j = 0; j < channels.size(); ++j) {
                            m.targets[j].positions.push_back(shapePos[j][vi]);
                            m.targets[j].normals.push_back(shapeNormal[j][vi]);
                        }
                        m.indices.push_back(unsigned(m.vertices.size()));
                        m.vertices.push_back(v);
                    }
                }
                if (!m.vertices.empty())
                    s.meshes.push_back(std::move(m));
            }
        }
        s.morphs = s.restMorphs();
        require(fbx->anim_stacks.count <= 256, "More than 256 FBX takes.");
        auto sourceClips = std::make_shared<std::vector<AnimationClip>>();
        qint64 sourceSamples = 0;
        for (auto stack : fbx->anim_stacks) {
            auto anim = stack->anim;
            double duration = anim->time_end - anim->time_begin;
            require(duration >= 0 && duration * s.fps <= 100000 &&
                        duration * s.fps * s.nodes.size() <= 5000000,
                    "FBX animation is too large to sample.");
            s.last = std::max(1, int(std::ceil(duration * s.fps)));
            for (int frame = 0; frame <= s.last; ++frame) {
                Fbx evaluated(ufbx_evaluate_scene(fbx.get(), anim, anim->time_begin + double(frame) / s.fps,
                                                  nullptr, &e),
                              ufbx_free_scene);
                require(bool(evaluated), "Cannot evaluate FBX animation.");
                Key key;
                key.morphs = s.restMorphs();
                for (auto n : evaluated->nodes)
                    key.pose.push_back(transform(n->local_transform));
                for (auto n : fbx->nodes)
                    if (n->mesh) {
                        std::vector<float> weights;
                        for (auto d : n->mesh->blend_deformers)
                            for (auto ch : d->channels)
                                for (auto blendKey : evaluated->blend_channels.data[ch->typed_id]->keyframes)
                                    weights.push_back(float(blendKey.effective_weight));
                        if (!weights.empty())
                            key.morphs[int(n->typed_id)] = std::move(weights);
                    }
                s.keys[frame] = std::move(key);
            }
            sourceSamples += qint64(s.keys.size()) * s.nodes.size();
            require(sourceSamples <= 5000000, "FBX takes exceed the sample memory budget.");
            AnimationClip clip;
            clip.name = str(stack->name);
            clip.duration = float(duration);
            clip.sampleFps = s.fps;
            clip.samples = s.keys;
            sourceClips->push_back(std::move(clip));
            s.keys.clear();
        }
        if (!sourceClips->empty()) {
            s.clips = sourceClips;
            require(s.selectClip(0, error), error);
            s.warnings << QString("FBX source rate: %1 fps; takes evaluated at %2 fps. Motion panel selects "
                                  "takes; Bake sets the output sampling rate.")
                              .arg(fbx->settings.frames_per_second)
                              .arg(s.fps);
        }
        s.warnings.removeDuplicates();
        require(s.validate(&error), error);
        scene = std::move(s);
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
bool loadCharacter(const QString &path, Scene &s, QString &error) {
    return QFileInfo(path).suffix().compare("fbx", Qt::CaseInsensitive) == 0 ? loadFbx(path, s, error)
                                                                             : loadGltf(path, s, error);
}
bool exportAnimation(const QString &path, const Scene &original, QString &error) {
    Scene s=original;s.compareOriginal=false;
    return QFileInfo(path).suffix().compare("fbx", Qt::CaseInsensitive) == 0 ? exportFbx(path, s, error)
                                                                             : exportGltf(path, s, error);
}
} // namespace trickster
