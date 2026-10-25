#include "io/project/project_io.h"
#include "core/scene/core.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace trickster;
Scene sphere() {
    Scene s;
    s.nodes = {{"Body", -1, {{0, 10, 0}, {}, {1, 1, 1}}, true, {}}};
    s.pose = s.restPose();
    s.bodies = {{0, 2, .1f, {}, {}}};
    s.keys[0] = {s.pose, {}, {}};
    s.last = 120;
    s.physics.substeps = 32;
    return s;
}
class PhysicsTests : public QObject {
    Q_OBJECT
  private slots:
    void massAndUnits() {
        auto s = sphere();
        s.nodes.push_back({"Other", -1, {{4, 10, 0}, {}, {1, 1, 1}}, true, {}});
        s.pose = s.restPose();
        s.keys.clear();
        s.bodies.push_back({1, 6, .1f, {}, {}});
        QVERIFY((physicalCenterOfMass(s) - QVector3D(3, 10, 0)).length() < 1e-6f);
        QCOMPARE(physicalMass(s), 8.f);
        QString error;
        QVERIFY2(s.validate(&error), qPrintable(error));
    }
    void gravityAndDeterminism() {
        auto s = sphere();
        Scene a, b;
        PhysicsReport ra, rb;
        QString error;
        QVERIFY2(simulatePhysics(s, 0, 30, a, ra, error), qPrintable(error));
        a.evaluate(30);
        QVERIFY2(std::abs(a.pose[0].t.y() - 5.095f) < .006f, qPrintable(QString::number(a.pose[0].t.y())));
        QVERIFY(simulatePhysics(s, 0, 30, b, rb, error));
        b.evaluate(30);
        QCOMPARE(a.pose[0].t, b.pose[0].t);
        s.physics.gravity = 0;
        QVERIFY(simulatePhysics(s, 0, 30, b, rb, error));
        b.evaluate(30);
        QVERIFY(std::abs(b.pose[0].t.y() - 10) < 1e-6f);
    }
    void groundCollision() {
        auto s = sphere();
        s.physics.ground = 2;
        Scene result;
        PhysicsReport report;
        QString error;
        QVERIFY2(simulatePhysics(s, 0, 120, result, report, error), qPrintable(error));
        result.evaluate(120);
        QVERIFY2(std::abs(result.pose[0].t.y() - 2.1f) < .003f,
                 qPrintable(QString::number(result.pose[0].t.y())));
        QVERIFY(report.maxPenetration < .02f);
    }
    void ballisticGravity() {
        auto s = sphere();
        s.last = 30;
        s.keys[30] = {s.pose, {}, {}};
        Scene result;
        PhysicsReport report;
        QString error;
        QVERIFY2(correctBallistic(s, 0, 30, result, report, error), qPrintable(error));
        result.evaluate(15);
        QVERIFY(std::abs(physicalCenterOfMass(result).y() - 11.22625f) < 1e-5f);
        QVERIFY(report.maxBallisticError < 1e-5f);
        for (int frame = 1; frame < 30; ++frame) {
            result.evaluate(frame - 1);
            auto a = physicalCenterOfMass(result);
            result.evaluate(frame);
            auto b = physicalCenterOfMass(result);
            result.evaluate(frame + 1);
            auto c = physicalCenterOfMass(result);
            QVERIFY(std::abs((c.y() - 2 * b.y() + a.y()) * 900 + 9.81f) < .004f);
        }
    }
    void constraintsAndProject() {
        auto s = Scene::mannequin();
        s.bodies = fittedPhysicsRig(s);
        s.physics.substeps = 16;
        for (auto &p : s.pose)
            p.t += QVector3D{};
        s.keys[0] = {s.pose, {}, {}};
        Scene result;
        PhysicsReport report;
        QString error;
        QVERIFY2(simulatePhysics(s, 0, 30, result, report, error), qPrintable(error));
        QVERIFY2(report.maxJointError < .01f, qPrintable(report.summary()));
        QTemporaryDir dir;
        QVERIFY(saveProject(dir.path() + "/physics.jst", result, error));
        Scene loaded;
        QVERIFY(loadProject(dir.path() + "/physics.jst", loaded, error));
        QCOMPARE(loaded.bodies.size(), result.bodies.size());
        QCOMPARE(loaded.physics.substeps, 16);
        QVERIFY(std::abs(physicalMass(loaded) - 70) < .001f);
    }
    void pinsAndTimedContacts() {
        auto s = sphere();
        s.pins[0] = {{0, 10, 0}, {0, 10, 1}, 1};
        s.keys[0].pins = s.pins;
        Scene result;
        PhysicsReport report;
        QString error;
        QVERIFY2(simulatePhysics(s, 0, 30, result, report, error), qPrintable(error));
        result.evaluate(30);
        QVERIFY((result.pose[0].t - QVector3D(0, 10, 0)).length() < .001f);
        QVERIFY(report.maxContactError < .001f);
        Scene chain;
        chain.nodes = {{"Root", -1, {{0, 5, 0}, {}, {1, 1, 1}}, true, {}},
                       {"Knee", 0, {{0, -1, 0}, {}, {1, 1, 1}}, true, {}},
                       {"Foot", 1, {{0, -1, 0}, {}, {1, 1, 1}}, true, {}}};
        chain.pose = chain.restPose();
        chain.last = 30;
        chain.bodies = {{0, 1, .1f, {0, -.5f, 0}, {0, -1, 0}},
                        {1, 1, .1f, {0, -.5f, 0}, {0, -1, 0}},
                        {2, 1, .1f, {}, {}}};
        chain.keys[0] = {chain.pose, {}, {}};
        chain.contacts = {{2, 0, 15, {0, 3, 0}, {0, 3, 1}}};
        QVERIFY2(simulatePhysics(chain, 0, 30, result, report, error), qPrintable(error));
        result.evaluate(10);
        QVERIFY2((result.positions(result.pose)[2] - QVector3D(0, 3, 0)).length() < .005f,
                 qPrintable(report.summary()));
        result.evaluate(30);
        QVERIFY(result.positions(result.pose)[2].y() < 2.95f);
        QCOMPARE(chain.contacts.size(), size_t(1));
        QVERIFY(result.contacts.empty());
    }
    void selfCollisions() {
        auto s = sphere();
        s.physics.gravity = 0;
        s.physics.selfCollision = true;
        s.nodes.push_back({"Second", -1, {{.15f, 10, 0}, {}, {1, 1, 1}}, true, {}});
        s.pose = s.restPose();
        s.keys.clear();
        s.keys[0] = {s.pose, {}, {}};
        s.bodies.push_back({1, 2, .1f, {}, {}});
        Scene result;
        PhysicsReport report;
        QString error;
        QVERIFY2(simulatePhysics(s, 0, 30, result, report, error), qPrintable(error));
        result.evaluate(30);
        QVERIFY((result.pose[0].t - result.pose[1].t).length() > .198f);
        QTemporaryDir dir;
        QVERIFY(saveProject(dir.path() + "/self.jst", result, error));
        Scene read;
        QVERIFY(loadProject(dir.path() + "/self.jst", read, error));
        QVERIFY(read.physics.selfCollision);
    }
    void rejectsInvalidPhysics() {
        auto s = sphere();
        s.physics.metersPerUnit = 0;
        QVERIFY(!s.validate());
        s.physics.metersPerUnit = 1;
        s.bodies[0].mass = -1;
        QVERIFY(!s.validate());
    }
};
QTEST_MAIN(PhysicsTests)
#include "physics_tests.moc"
