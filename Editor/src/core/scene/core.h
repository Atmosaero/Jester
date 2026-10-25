#pragma once
#include "core/animation/animation_curves.h"
#include "physics/physics.h"
#include <QByteArray>
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector2D>
#include <QVector3D>
#include <array>
#include <memory>
#include <set>
#include <vector>

namespace trickster {
struct Transform {
    QVector3D t;
    QQuaternion r;
    QVector3D s{1, 1, 1};
    QMatrix4x4 matrix() const;
};
using Pose = std::vector<Transform>;
struct Node {
    QString name;
    int parent = -1;
    Transform rest;
    bool joint = true;
    std::vector<float> morphRest;
};
struct Vertex {
    QVector3D p;
    std::array<int, 8> joints{};
    std::array<float, 8> weights{{1, 0, 0, 0, 0, 0, 0, 0}};
    QVector3D normal;
    QVector2D uv, uv1;
    QVector4D tangent;
};
struct Texture {
    QByteArray bytes;
    QString mime = "image/png";
    int coord = 0, wrapS = 10497, wrapT = 10497, minFilter = 9987, magFilter = 9729;
    QVector2D offset, scale{1, 1};
    float rotation = 0;
};
struct MorphTarget {
    QString name;
    std::vector<QVector3D> positions, normals, tangents;
};
using MorphWeights = QMap<int, std::vector<float>>;
struct Mesh {
    int node = -1;
    std::vector<Vertex> vertices;
    std::vector<unsigned> indices;
    std::vector<int> joints;
    std::vector<QMatrix4x4> inverseBind;
    QColor color{109, 156, 179};
    QMap<QString, Texture> textures;
    std::vector<MorphTarget> targets;
    float metallic = 0, roughness = .75f, alphaCutoff = .5f, normalScale = 1, occlusionStrength = 1;
    QVector3D emissive;
    QString alphaMode = "OPAQUE";
    bool doubleSided = true, unlit = false;
};
struct Pin {
    QVector3D target;
    QVector3D pole;
    float weight = 1;
};
struct Key {
    Pose pose;
    QMap<int, Pin> pins;
    MorphWeights morphs;
    QString id; // optional stable bundle identity, allocated on context attachment
};
struct AnimationChannel {
    int node = -1, components = 3;
    QString path, interpolation = "LINEAR";
    std::vector<float> times, values;
};
struct AnimationClip {
    QString name;
    float duration = 0;
    int sampleFps = 30;
    std::vector<AnimationChannel> channels;
    QMap<int, Key> samples;
};
// Ordered additive layers. Sparse per-node key tracks leave unedited bones on the source.
struct CorrectionKey {
    QMap<int, Transform> offsets;
    MorphWeights morphOffsets;
    QMap<int, int> channels; // bit 1 position, 2 rotation, 4 scale; absent means legacy TRS (7)
    QString id;
};
struct LayerTrackIndex;
struct MotionCorrection {
    QString id, name;
    QMap<int, CorrectionKey> keys;
    mutable std::shared_ptr<const LayerTrackIndex> trackIndex;
    bool present = false, enabled = true, editing = true, world = false;
    int first = 0, last = 30, fadeIn = 5, fadeOut = 5;
    float strength = 1;
    Pose basePose;
    MorphWeights baseMorphs;
    QMap<int, Pin> basePins, pins;
    std::set<int> removedPins;
    QMap<int, Transform> offsets;
    MorphWeights morphOffsets;
};
struct Contact {
    int node = -1, first = 0, last = 30;
    QVector3D target, pole;
    QQuaternion orientation;
    bool orient = false;
    int fadeIn = 0, fadeOut = 0;
};
float contactWeight(const Contact &contact, float frame);

struct ContactReport {
    float position = 0, angle = 0;
    int samples = 0;
    QStringList warnings;
};
struct IKChain {
    int root = -1, mid = -1, end = -1;
    QVector3D pole{0, 0, 1}; // scene-space direction in the rest pose
};
struct CharacterProfile {
    bool configured = false;
    QMap<QString, int> roles;
    QMap<int, IKChain> chains;
    QMap<int, int> mirrors;
};
struct Balance {
    QVector3D center;
    std::vector<QVector3D> support;
    bool supported = false;
};
struct Scene {
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    Pose pose;
    MorphWeights morphs;
    QMap<int, Key> keys;
    QMap<int, Pin> pins;
    int first = 0, last = 90, fps = 30;
    QString name = "Untitled scene";
    QJsonObject retargetProfile;
    std::vector<ScalarCurve> curves;
    Pose curveBasePose;
    MorphWeights curveBaseMorphs;
    QMap<int, Pin> curveBasePins;
    QJsonArray annotations; // stable IDs; double seconds, independent of display FPS
    float posingRestHeight = -1; // derived snapshot input, never persisted
    bool bodyAssist = false;
    QMap<int, float> rotationLimits;
    QMap<int, JointLimit> jointLimits;
    std::vector<Contact> contacts;
    float sampleFrame = 0;
    double sourceEvaluationMs = 0, curveEvaluationMs = 0, layerEvaluationMs = 0, constraintEvaluationMs = 0;
    QStringList warnings;
    std::shared_ptr<const std::vector<AnimationClip>> clips;
    int sourceClip = -1;
    CharacterProfile profile;
    std::vector<MotionCorrection> layers;
    int activeLayer = -1;
    bool compareOriginal = false;
    MotionCorrection emptyCorrection;
    MotionCorrection &correction() {
        return activeLayer >= 0 && activeLayer < int(layers.size()) ? layers[activeLayer] : emptyCorrection;
    }
    const MotionCorrection &correction() const {
        return activeLayer >= 0 && activeLayer < int(layers.size()) ? layers[activeLayer] : emptyCorrection;
    }
    void clearCorrections();
    void removeLayer(int index);
    float layerWeight(const MotionCorrection &layer, float frame) const;
    CorrectionKey sampleLayer(const MotionCorrection &layer, float frame) const;
    void applyLayer(const MotionCorrection &layer);
    Scene resampledCopy(int outputFps) const;

    PhysicsSettings physics;
    QJsonObject physicsResult; // persisted provenance/metrics; no native engine state
    std::vector<RigidSegment> bodies;
    Pose restPose() const;
    MorphWeights restMorphs() const;
    std::vector<float> morphWeights(int node) const;
    std::vector<QVector3D> skinnedNormals(const Mesh &mesh) const;
    std::vector<QMatrix4x4> globals(const Pose &p) const;
    std::vector<QVector3D> positions(const Pose &p) const;
    bool isEffector(int node) const;
    CharacterProfile suggestedProfile() const;
    bool validateProfile(QString *error = nullptr) const;
    IKChain ikChain(int end) const;
    QString ikDiagnostic(int end) const;
    bool isFoot(int node) const;
    bool solveIK(int end, const QVector3D &target, const QVector3D &pole, float weight = 1);
    QVector3D defaultPole(int end) const;
    void solvePins(float softEnvelope = 1);
    void enforceLimits();
    ContactReport diagnoseContacts() const;
    bool reach(int end, const QVector3D &target, const QVector3D &pole, float weight = 1);
    int mirrorPartner(int node) const;
    bool sideNamed(int node) const;
    bool copyToOpposite(const std::vector<int> &sources);
    bool copyToOpposite(const QMap<int, int> &components, bool mirrorPins);
    void mirrorChanged(int source, const Pose &before);
    void mirrorPose();
    Balance balance(float characterHeight = 0) const;
    Scene motionOnlyCopy() const;
    Scene bakedCopy() const;
    Scene ballisticCopy(int start, int end, float apexHeight) const;
    void evaluate(float frame);
    float evaluateSource(float frame);
    bool evaluateNativeSource(float frame);
    bool selectClip(int index, QString &error);
    bool validateClips(QString *error = nullptr) const;
    float correctionWeight(float frame) const;
    void applyCorrection();
    bool captureCorrection(const Pose &before, const MorphWeights &beforeMorphs, QString &error,
                           const QMap<int, Pin> *beforePins = nullptr);
    bool beginCorrection(int start, int end, int fadeIn, int fadeOut, bool world, QString &error);
    QQuaternion globalRotation(int node, const Pose &p) const;
    bool validateCorrection(QString *error = nullptr) const;
    std::vector<QVector3D> skinned(const Mesh &mesh) const;
    std::vector<QVector3D> skinned(const Mesh &mesh, const std::vector<QMatrix4x4> &globals) const;
    std::vector<QVector3D> skinnedNormals(const Mesh &mesh, const std::vector<QMatrix4x4> &globals) const;
    void translateWorld(int node, const QVector3D &delta);
    void rotate(int node, const QVector3D &axis, float degrees, bool world);
    float height() const;
    std::pair<QVector3D, QVector3D> bounds(bool rest = false) const;
    bool validate(QString *error = nullptr) const;
    static Scene mannequin();
};
void completeGeometryBasis(Mesh &mesh);
std::vector<double> animationExportTimes(const Scene &scene);
QSet<QString> steppedExportChannels(const Scene &scene);
Transform interpolate(const Transform &a, const Transform &b, float t);
QJsonObject sceneToJson(const Scene &scene);
bool sceneFromJson(const QJsonObject &object, Scene &scene, QString &error);
bool loadFbx(const QString &path, Scene &scene, QString &error);
bool exportFbx(const QString &path, const Scene &scene, QString &error);
bool loadCharacter(const QString &path, Scene &scene, QString &error);
bool exportAnimation(const QString &path, const Scene &scene, QString &error);
bool loadGltf(const QString &path, Scene &scene, QString &error);
bool exportGltf(const QString &path, const Scene &scene, QString &error);
bool saveProject(const QString &path, const Scene &scene, QString &error);
bool loadProject(const QString &path, Scene &scene, QString &error);
} // namespace trickster
