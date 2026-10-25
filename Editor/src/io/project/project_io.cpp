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

#include "io/project/project_io.h"
#include "io/project/json_values.h"
namespace trickster {
using namespace project_detail;
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
            {"version", 9},
            {"visuals",s.visuals.toJson()},
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
        need(o["format"] == "Trickster" && (o["version"].toInt() >= 1 && o["version"].toInt() <= 9),
             "Unsupported Jester project version.");
        Scene s;
        if(o.contains("visuals")) {
            need(o["visuals"].isObject(),"Invalid Jester scene visual settings.");
            QString visualError;
            need(SceneVisualSettings::fromJson(o["visuals"].toObject(),s.visuals,visualError),visualError);
        }
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
    if (!isNativeProjectFile(path)) {
        error="Jester project paths must use one .jst extension.";
        return false;
    }
    if (!s.validate(&error))
        return false;
    return writeFile(path, QJsonDocument(sceneToJson(s)).toJson(QJsonDocument::Compact), error);
}
bool loadProject(const QString &path, Scene &s, QString &error) {
    if (!isNativeProjectFile(path)) {
        error="Jester project paths must use one .jst extension.";
        return false;
    }
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

} // namespace trickster
