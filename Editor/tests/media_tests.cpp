#include "core.h"
#include "document.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace trickster;
Scene mediaFixture() {
    Scene s;
    s.name = "Media fixture";
    s.last = 30;
    s.nodes = {{"Root", -1, {}, true, {0}}, {"Joint", 0, {{0, .5f, 0}, {}, {1, 1, 1}}, true, {}}};
    s.pose = s.restPose();
    Mesh m;
    m.node = 0;
    m.color = Qt::white;
    m.joints = {0, 1};
    auto g = s.globals(s.pose);
    m.inverseBind = {g[0].inverted(), g[1].inverted()};
    for (auto p : std::vector<QVector3D>{{-.5f, 0, 0}, {.5f, 0, 0}, {.5f, 1, 0}, {-.5f, 1, 0}}) {
        Vertex v;
        v.p = p;
        v.normal = {0, 0, 1};
        v.uv = {p.x() + .5f, 1 - p.y()};
        v.weights = {.5f, 0, 0, 0, .5f, 0, 0, 0};
        v.joints[4] = 1;
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    MorphTarget target;
    target.name = "Smile";
    target.positions = {{0, 0, 0}, {0, 0, 0}, {.2f, .3f, 0}, {-.2f, .3f, 0}};
    target.normals.resize(4);
    m.targets.push_back(target);
    QImage im(2, 2, QImage::Format_RGBA8888);
    im.setPixelColor(0, 0, Qt::red);
    im.setPixelColor(1, 0, Qt::green);
    im.setPixelColor(0, 1, Qt::blue);
    im.setPixelColor(1, 1, Qt::yellow);
    Texture texture;
    QBuffer buf(&texture.bytes);
    buf.open(QIODevice::WriteOnly);
    im.save(&buf, "PNG");
    texture.minFilter = 9728;
    texture.magFilter = 9728;
    m.textures["baseColor"] = texture;
    m.textures["normal"] = texture;
    m.textures["metallicRoughness"] = texture;
    m.metallic = .3f;
    m.roughness = .4f;
    s.meshes.push_back(m);
    s.morphs = s.restMorphs();
    s.keys[0] = {s.pose, {}, s.morphs};
    s.morphs[0] = {1};
    s.pose[1].r = QQuaternion::fromEulerAngles(15, 20, 25);
    s.pose[0].t = {.1f, .1f, .1f};
    s.keys[30] = {s.pose, {}, s.morphs};
    s.evaluate(0);
    return s;
}
class MediaTests : public QObject {
    Q_OBJECT
  private slots:
    void projectAndGltfRoundtrip() {
        auto s = mediaFixture();
        QString error;
        QVERIFY2(s.validate(&error), qPrintable(error));
        QTemporaryDir dir;
        for (QString ext : {"trickster", "glb", "gltf"}) {
            auto path = dir.path() + "/media." + ext;
            QVERIFY2(ext == "trickster" ? saveProject(path, s, error) : exportGltf(path, s, error),
                     qPrintable(error));
            Scene loaded;
            QVERIFY2(ext == "trickster" ? loadProject(path, loaded, error) : loadGltf(path, loaded, error),
                     qPrintable(error));
            QCOMPARE(loaded.meshes.size(), size_t(1));
            const auto &m = loaded.meshes[0];
            QCOMPARE(m.targets.size(), size_t(1));
            QCOMPARE(m.targets[0].name, QString("Smile"));
            QCOMPARE(m.textures.size(), 3);
            QCOMPARE(m.textures["baseColor"].bytes, s.meshes[0].textures["baseColor"].bytes);
            QVERIFY(std::abs(m.vertices[0].weights[4] - .5f) < 1e-6f);
            for (int frame : {0, 7, 15, 30}) {
                s.evaluate(float(frame));
                loaded.evaluate(float(frame));
                auto a = s.skinned(s.meshes[0]), b = loaded.skinned(m);
                for (size_t i = 0; i < a.size(); ++i)
                    QVERIFY2((a[i] - b[i]).length() < .0001f,
                             qPrintable(QString("%1 frame %2 vertex %3").arg(ext).arg(frame).arg(i)));
            }
            if (ext == "glb") {
                QFile::remove("artifacts/media-roundtrip.glb");
                QFile::copy(path, "artifacts/media-roundtrip.glb");
            }
        }
    }
    void undoMorphAndTabs() {
        Document d;
        d.replace(mediaFixture());
        d.seek(15);
        auto original = d.scene.morphWeights(0)[0];
        d.edit("Morph", [](Scene &s) { s.morphs[0] = {.8f}; });
        QVERIFY(std::abs(d.scene.keys[15].morphs[0][0] - .8f) < 1e-6f);
        auto session = d.captureSession();
        d.replace(Scene{});
        d.restoreSession(session);
        d.undo.undo();
        QVERIFY(std::abs(d.scene.morphWeights(0)[0] - original) < 1e-6f);
        d.undo.redo();
        QVERIFY(std::abs(d.scene.morphWeights(0)[0] - .8f) < 1e-6f);
    }
    void fbxRoundtrip() {
        auto s = mediaFixture();

        auto &tex = s.meshes[0].textures["baseColor"];
        tex.offset = {.2f, .1f};
        tex.scale = {2, 2};
        tex.rotation = .3f;
        tex.coord = 1;
        for (auto &v : s.meshes[0].vertices)
            v.uv1 = v.uv;
        QTemporaryDir dir;
        QString error;
        auto path = dir.path() + "/media.fbx";
        QVERIFY2(exportFbx(path, s, error), qPrintable(error));
        QFile::remove("artifacts/media.fbx");
        QFile::copy(path, "artifacts/media.fbx");
        Scene loaded;
        QVERIFY2(loadFbx(path, loaded, error), qPrintable(error));
        QCOMPARE(loaded.meshes.size(), size_t(1));
        QCOMPARE(loaded.meshes[0].targets.size(), size_t(1));
        QCOMPARE(loaded.meshes[0].textures["baseColor"].bytes, s.meshes[0].textures["baseColor"].bytes);
        QCOMPARE(loaded.meshes[0].textures.size(), 3);
        const auto &readTex = loaded.meshes[0].textures["baseColor"];
        QCOMPARE(readTex.coord, 1);
        QVERIFY((readTex.offset - tex.offset).length() < 1e-5f);
        QVERIFY((readTex.scale - tex.scale).length() < 1e-5f);
        QVERIFY(std::abs(readTex.rotation - tex.rotation) < 1e-5f);
        QVERIFY(std::abs(loaded.meshes[0].metallic - .3f) < 1e-5f);
        QVERIFY(std::abs(loaded.meshes[0].roughness - .4f) < 1e-5f);
        QCOMPARE(loaded.fps, 30);
        QCOMPARE(loaded.last, 30);
        for (int frame : {0, 7, 15, 30}) {
            s.evaluate(float(frame));
            loaded.evaluate(float(frame));
            auto a = s.skinned(s.meshes[0]), b = loaded.skinned(loaded.meshes[0]);
            QCOMPARE(b.size(), s.meshes[0].indices.size());
            for (size_t i = 0; i < b.size(); ++i)
                QVERIFY2((a[s.meshes[0].indices[i]] - b[i]).length() < .001f,
                         qPrintable(QString("frame %1 corner %2 error %3")
                                        .arg(frame)
                                        .arg(i)
                                        .arg((a[s.meshes[0].indices[i]] - b[i]).length())));
        }
        QVERIFY2(exportGltf("artifacts/fbx-roundtrip.glb", loaded, error), qPrintable(error));
    }
    void officialMediaFixtures() {
        for (QString name : {"BoxTextured", "AnimatedMorphCube", "DamagedHelmet"}) {
            auto path = "artifacts/external/" + name + ".glb";
            if (!QFile::exists(path))
                QSKIP("Official optional fixtures absent");
            Scene s, loaded;
            QString error;
            QVERIFY2(loadGltf(path, s, error), qPrintable(name + ": " + error));
            auto out = "artifacts/" + name + "-roundtrip.glb";
            QVERIFY2(exportGltf(out, s, error), qPrintable(error));
            QVERIFY2(loadGltf(out, loaded, error), qPrintable(error));
            QCOMPARE(loaded.meshes.size(), s.meshes.size());
            if (name == "AnimatedMorphCube") {
                QVERIFY(!s.keys.isEmpty());
                QVERIFY(!s.meshes[0].targets.empty());
                s.evaluate(0);
                auto a = s.skinned(s.meshes[0]);
                s.evaluate(s.last * .5f);
                auto b = s.skinned(s.meshes[0]);
                bool moved = false;
                for (size_t i = 0; i < a.size(); ++i)
                    moved = moved || (a[i] - b[i]).length() > .001f;
                QVERIFY(moved);
            } else {
                QVERIFY(!s.meshes[0].textures.isEmpty());
                QCOMPARE(s.meshes[0].textures["baseColor"].bytes,
                         loaded.meshes[0].textures["baseColor"].bytes);
            }
        }
    }
    void textureTransformRoundtrip() {
        auto s = mediaFixture();
        auto &tex = s.meshes[0].textures["baseColor"];
        tex.offset = {.2f, .3f};
        tex.scale = {2, 3};
        tex.rotation = .4f;
        s.meshes[0].unlit = true;
        QString error;
        QTemporaryDir dir;
        for (QString ext : {"trickster", "glb"}) {
            auto path = dir.path() + "/transform." + ext;
            QVERIFY(ext == "glb" ? exportGltf(path, s, error) : saveProject(path, s, error));
            Scene read;
            QVERIFY2(ext == "glb" ? loadGltf(path, read, error) : loadProject(path, read, error),
                     qPrintable(error));
            const auto &t = read.meshes[0].textures["baseColor"];
            QCOMPARE(t.offset, tex.offset);
            QCOMPARE(t.scale, tex.scale);
            QCOMPARE(t.rotation, tex.rotation);
            QVERIFY(read.meshes[0].unlit);
            if (ext == "glb") {
                QFile::remove("artifacts/texture-transform.glb");
                QFile::copy(path, "artifacts/texture-transform.glb");
            }
        }
    }
    void invalidMorphAndTextureData() {
        auto s = mediaFixture();
        s.meshes[0].node = 999;
        QVERIFY(!s.validate());
        s = mediaFixture();
        s.meshes[0].textures["baseColor"].wrapS = 42;
        QVERIFY(!s.validate());
        s = mediaFixture();
        s.keys[0].morphs[0] = {0, 1};
        QVERIFY(!s.validate());
        s = mediaFixture();
        s.meshes[0].targets[0].positions.pop_back();
        QVERIFY(!s.validate());
    }
    void suppliedNativeFbx() {
        QString path = qEnvironmentVariable("TRICKSTER_TEST_FBX");
        if (!QFile::exists(path))
            QSKIP("Local FBX absent");
        Scene s;
        QString error;
        QVERIFY2(loadFbx(path, s, error), qPrintable(error));
        QVERIFY(s.nodes.size() > 40);
        QVERIFY(!s.meshes.empty());
        QVERIFY(s.height() > .5f && s.height() < 5.f);
        QVERIFY2(s.validate(&error), qPrintable(error));
        QVERIFY2(exportGltf("artifacts/native-fbx.glb", s, error), qPrintable(error));
        s.last = 2;
        s.keys.clear();
        s.keys[0] = {s.pose, {}, s.morphs};
        for (size_t i = 0; i < s.nodes.size(); ++i)
            if (s.nodes[i].name.contains("hand", Qt::CaseInsensitive)) {
                s.pose[i].r = (s.pose[i].r * QQuaternion::fromEulerAngles(10, 15, 20)).normalized();
                break;
            }
        s.keys[2] = {s.pose, {}, s.morphs};
        QVERIFY2(exportFbx("artifacts/native-edited.fbx", s, error), qPrintable(error));
        Scene read;
        QVERIFY2(loadFbx("artifacts/native-edited.fbx", read, error), qPrintable(error));
        QCOMPARE(read.meshes.size(), s.meshes.size());
        s.evaluate(1);
        read.evaluate(1);
        for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
            auto a = s.skinned(s.meshes[mi]), b = read.skinned(read.meshes[mi]);
            QCOMPARE(b.size(), s.meshes[mi].indices.size());
            for (size_t i = 0; i < b.size(); ++i)
                QVERIFY2((a[s.meshes[mi].indices[i]] - b[i]).length() < .0002f,
                         qPrintable(QString("Native FBX mesh %1 corner %2").arg(mi).arg(i)));
        }
    }
};
QTEST_MAIN(MediaTests)
#include "media_tests.moc"
