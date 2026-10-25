#include "appearance.h"
#include "editor_api.h"
#include "physics_demos.h"
#include "window.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QScrollArea>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace trickster;
class PhysicsUI : public QObject {
    Q_OBJECT
    void visible(Window &w, QWidget *widget) {
        for (auto p = widget->parentWidget(); p; p = p->parentWidget())
            if (auto d = qobject_cast<QDockWidget *>(p)) {
                d->show();
                d->raise();
            }
        for (auto p = widget->parentWidget(); p; p = p->parentWidget())
            if (auto a = qobject_cast<QScrollArea *>(p))
                a->ensureWidgetVisible(widget);
        QTest::qWait(30);
    }
    void clean(Window &w) {
        for (auto d : w.workspace.documents)
            d->undo.setClean();
        w.close();
    }
  private slots:
    void initTestCase() {
        QSettings().clear();
        QDir().mkpath("artifacts/physics-ui");
    }
    void compactWholeBodyAndUndo() {
        Window w;
        auto meshOnly = Scene::mannequin();
        for (auto &node : meshOnly.nodes)
            node.joint = false;
        w.document().replace(meshOnly);
        w.showWholeBodyDialog();
        QVERIFY(!w.findChild<QDialog *>("wholeBodyDialog"));
        w.document().replace(physicsDemo("whole-body"));
        w.document().select(8);
        double scale = w.devicePixelRatioF();
        w.resize(qRound(1280 / scale), qRound(720 / scale));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(60);
        QVERIFY2(w.width() * scale <= 1281, qPrintable(QString("physical width %1").arg(w.width() * scale)));
        QVERIFY2(w.height() * scale <= 721,
                 qPrintable(QString("physical height %1").arg(w.height() * scale)));
        auto button = w.findChild<QPushButton *>("wholeBodyButton");
        visible(w, button);
        QVERIFY(button->isVisible());
        auto original = w.document().scene.pose;
        int undo = w.document().undo.count();
        bool seen = false;
        QTimer::singleShot(100, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            seen = true;
            auto table = dialog->findChild<QTableWidget *>("wholeBodyGoals");
            QCOMPARE(table->rowCount(), 3);
            auto y = qobject_cast<QDoubleSpinBox *>(table->cellWidget(2, 5));
            y->setValue(y->value() + .15);
            qobject_cast<QSpinBox *>(table->cellWidget(2, 2))->setValue(1);
            qobject_cast<QDoubleSpinBox *>(table->cellWidget(2, 3))->setValue(2);
            auto solve = dialog->findChild<QPushButton *>("solveWholeBodyButton");
            QVERIFY(solve->isVisible());
            QTest::mouseClick(solve, Qt::LeftButton);
            QVERIFY(dialog->findChild<QLabel *>("wholeBodyReport")->text().contains("solved"));
            QVERIFY(
                dialog->grab().save("artifacts/physics-ui/whole-body-" + QString::number(scale) + ".png"));
            dialog->reject();
        });
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(seen);
        QCOMPARE(w.document().undo.count(), undo + 1);
        QVERIFY(w.document().scene.correction().present);
        QVERIFY(w.document().scene.correction().keys.contains(0));
        QTimer::singleShot(80, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            QTimer::singleShot(10000, dialog, &QDialog::reject);
            auto table = dialog->findChild<QTableWidget *>("wholeBodyGoals");
            QCOMPARE(table->rowCount(), 3);
            QCOMPARE(qobject_cast<QSpinBox *>(table->cellWidget(2, 2))->value(), 1);
            QCOMPARE(qobject_cast<QDoubleSpinBox *>(table->cellWidget(2, 3))->value(), 2.);
            QTest::mouseClick(dialog->findChild<QPushButton *>("addWholeBodyGoal"), Qt::LeftButton);
            QCOMPARE(table->rowCount(), 4);
            QVERIFY(!table->cellWidget(0, 7)->visibleRegion().isEmpty());
            QTest::mouseClick(qobject_cast<QToolButton *>(table->cellWidget(0, 7)), Qt::LeftButton);
            QCOMPARE(table->rowCount(), 3);
            auto combo = qobject_cast<QComboBox *>(table->cellWidget(0, 0));
            combo->setCurrentIndex(combo->findData(0));
            QVERIFY(std::abs(qobject_cast<QDoubleSpinBox *>(table->cellWidget(0, 4))->value() -
                             w.document().scene.positions(w.document().scene.pose)[0].x()) < 1e-5);
            dialog->reject();
        });
        w.showWholeBodyDialog();
        QCOMPARE(w.document().undo.count(), undo + 1);
        w.document().undo.undo();
        for (int i = 0; i < int(original.size()); ++i)
            QVERIFY((original[i].t - w.document().scene.pose[i].t).length() < 1e-6);
        w.viewport->showPhysicsModel = true;
        QVERIFY(captureEditorWidget(&w).save("artifacts/physics-ui/workspace-" + QString::number(scale) + ".png"));
        clean(w);
    }
    void modelAndMcpAnalysis() {
        QTemporaryDir dir;
        Window w;
        auto original = physicsDemo("whole-body");
        original.bodies.back().inertia = {.000001000001f, .000001000002f, .000001000003f};
        w.document().replace(original);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        bool seen = false;
        int undo = w.document().undo.count();
        QTimer::singleShot(80, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            seen = true;
            auto table = dialog->findChild<QTableWidget *>("physicsModelTable");
            QVERIFY(table->rowCount() > 10);
            QTimer::singleShot(10000, dialog, &QDialog::reject);
            qobject_cast<QDoubleSpinBox *>(table->cellWidget(0, 1))->setValue(5);
            QTest::mouseClick(dialog->findChild<QPushButton *>("applyPhysicsModel"), Qt::LeftButton);
            QTimer chooseFile;
            chooseFile.setInterval(30);
            connect(&chooseFile, &QTimer::timeout, dialog, [&] {
                for (auto widget : QApplication::topLevelWidgets())
                    if (auto file = qobject_cast<QFileDialog *>(widget)) {
                        if (!file->isVisible())
                            continue;
                        file->setOption(QFileDialog::DontConfirmOverwrite, true);
                        file->setDirectory(dir.path());
                        file->selectFile("ui-model.xml");
                        if (auto edit = file->findChild<QLineEdit *>("fileNameEdit"))
                            edit->setText("ui-model.xml");
                        QMetaObject::invokeMethod(file, "accept", Qt::QueuedConnection);
                        chooseFile.stop();
                        break;
                    }
            });
            chooseFile.start();
            QTimer::singleShot(12000, dialog, [] {
                for (auto widget : QApplication::topLevelWidgets())
                    if (auto file = qobject_cast<QFileDialog *>(widget))
                        file->reject();
            });
            QTest::mouseClick(dialog->findChild<QPushButton *>("exportMjcfButton"), Qt::LeftButton);
            chooseFile.stop();
            QVERIFY(QFile::exists(dir.path() + "/ui-model.xml"));
            QVERIFY(dialog->grab().save("artifacts/physics-ui/model-" +
                                        QString::number(w.devicePixelRatioF()) + ".png"));
            dialog->reject();
        });
        w.physicsModelDialog();
        QVERIFY(seen);
        QCOMPARE(w.document().undo.count(), undo + 1);
        QCOMPARE(w.document().scene.bodies[0].mass, 5.f);
        QCOMPARE(w.document().scene.bodies[1].mass, original.bodies[1].mass);
        QCOMPARE(w.document().scene.bodies.back().inertia, original.bodies.back().inertia);
        w.document().undo.undo();
        EditorApi api(&w);
        auto report = api.call("physics_analyze", {{"operation", "measure"}});
        QVERIFY2(!report["isError"].toBool(), qPrintable(QString::fromUtf8(QJsonDocument(report).toJson())));
        auto xml = api.call("physics_analyze", {{"operation", "mjcf"}, {"path", dir.path() + "/model.xml"}});
        QVERIFY(!xml["isError"].toBool());
        QVERIFY(QFile::exists(dir.path() + "/model.xml"));
        clean(w);
    }
    void modelRigOperationsAndReport() {
        Window w;
        w.document().replace(physicsDemo("whole-body"));
        double scale = w.devicePixelRatioF();
        w.resize(qRound(1280 / scale), qRound(720 / scale));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        int undo = w.document().undo.count();
        bool seen = false;
        QTimer::singleShot(100, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            QTimer::singleShot(10000, dialog, &QDialog::reject);
            seen = true;
            QVERIFY(dialog->width() * scale <= 1281);
            QVERIFY(dialog->height() * scale <= 721);
            QTest::mouseClick(dialog->findChild<QPushButton *>("fitHumanPhysicsRigButton"), Qt::LeftButton);
            auto table = dialog->findChild<QTableWidget *>("physicsModelTable");
            int count = table->rowCount();
            QVERIFY(count >= 15);
            QTest::mouseClick(dialog->findChild<QPushButton *>("fitFootPadsButton"), Qt::LeftButton);
            int foot = -1;
            for (const auto &body : w.document().scene.bodies)
                if (body.footHalfSize.lengthSquared() > 0)
                    foot = body.node;
            QVERIFY(foot >= 0);
            table->selectRow(count - 1);
            int removed = w.document().scene.bodies.back().node;
            QTest::mouseClick(dialog->findChild<QPushButton *>("physicsRemoveBody"), Qt::LeftButton);
            QCOMPARE(table->rowCount(), count - 1);
            auto nodes = dialog->findChild<QComboBox *>("physicsNewBodyNode");
            nodes->setCurrentIndex(nodes->findData(removed));
            QTest::mouseClick(dialog->findChild<QPushButton *>("physicsAddBody"), Qt::LeftButton);
            QCOMPARE(table->rowCount(), count);
            QVERIFY(dialog->grab().save("artifacts/physics-ui/model-rig-" + QString::number(scale) + ".png"));
            dialog->reject();
        });
        w.physicsModelDialog();
        QVERIFY(seen);
        QCOMPARE(w.document().undo.count(), undo + 4);
        while (w.document().undo.canUndo())
            w.document().undo.undo();
        clean(w);
    }
    void dynamicsSettingsAndReport() {
        Window w;
        w.document().replace(physicsDemo("landing"));
        double scale = w.devicePixelRatioF();
        w.resize(qRound(1280 / scale), qRound(720 / scale));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        bool seen = false;
        QTimer::singleShot(100, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            QTimer::singleShot(10000, dialog, &QDialog::reject);
            seen = true;
            QVERIFY(dialog->findChild<QCheckBox *>("physicsEndAtRest")->isChecked());
            auto preparation = dialog->findChild<QCheckBox *>("physicsPrepareLanding");
            QVERIFY(preparation);
            QVERIFY(preparation->isChecked());
            preparation->setChecked(false);
            QVERIFY(dialog->findChild<QLineEdit *>("solverImportantFrames")->text().contains("0:10"));
            QVERIFY(!dialog->findChild<QComboBox *>("flightBoundaryMode")->isVisible());
            QVERIFY(!dialog->findChild<QCheckBox *>("flightConserveAngular")->isVisible());
            QVERIFY(dialog->height() * scale <= 721);
            QVERIFY(dialog->width() * scale <= 1281);
            QVERIFY(dialog->grab().save("artifacts/physics-ui/dynamics-" + QString::number(scale) + ".png"));
            // Exercise the same background job as MCP with a bounded interactive budget.
            dialog->findChild<QSpinBox *>("solverIterations")->setValue(2);
            QTest::mouseClick(dialog->findChild<QPushButton *>("calculatePhysicsButton"), Qt::LeftButton);
        });
        w.physicsCalculationDialog("optimize");
        QVERIFY(seen);
        QTRY_VERIFY_WITH_TIMEOUT(w.hasPhysicsResult(), 15000);
        QVERIFY(w.physicsResult()->physicsResult["endAtRest"].toBool());
        QVERIFY(!w.physicsResult()->physicsResult["prepareLanding"].toBool(true));
        QVERIFY(!w.physicsResult()->physics.requests["optimize"].toObject()["prepareLanding"].toBool(true));
        QTimer::singleShot(100, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            QTimer::singleShot(10000, dialog, &QDialog::reject);
            auto metrics = dialog->findChild<QTableWidget *>("physicsReportTable");
            QVERIFY(!metrics->findItems("End COM speed (m/s)", Qt::MatchExactly).isEmpty());
            QVERIFY(!metrics->findItems("Impulse balance force RMS (N)", Qt::MatchExactly).isEmpty());
            QVERIFY(dialog->height() * scale <= 721);
            QVERIFY(dialog->grab().save("artifacts/physics-ui/report-" + QString::number(scale) + ".png"));
            auto peak = metrics->findItems("Peak slip frame", Qt::MatchExactly);
            QVERIFY(!peak.empty());
            metrics->scrollToItem(peak.front());
            auto point = metrics->visualItemRect(peak.front()).center();
            QTest::mouseClick(metrics->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseDClick(metrics->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QCOMPARE(w.document().frame, qRound(peak.front()->data(Qt::UserRole).toDouble()));
            dialog->reject();
        });
        w.physicsReportDialog();
        clean(w);
    }
    void portableComparisons() {
        Window w;
        double scale = w.devicePixelRatioF();
        w.resize(qRound(1280 / scale), qRound(720 / scale));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        for (QString kind : {"whole-body", "com", "jump", "tuck", "squat", "landing"}) {
            QString error;
            Scene source, result;
            QVERIFY2(loadProject("examples/physics/" + kind + "-source.trickster", source, error),
                     qPrintable(error));
            QVERIFY2(loadProject("examples/physics/" + kind + "-result.trickster", result, error),
                     qPrintable(error));
            w.document().replace(source);
            w.document().seek((source.first + source.last) / 2);
            w.viewport->showPhysicsModel = true;
            w.viewport->showBalance = true;
            w.viewport->showTrajectory = true;
            w.viewport->physicsPreview = result;
            w.viewport->physicsDiagnostics = result.physicsResult;
            w.viewport->fit();
            w.viewport->update();
            QTest::qWait(120);
            QVERIFY(captureEditorWidget(&w).save("artifacts/physics-ui/compare-" + kind + "-" + QString::number(scale) +
                                  ".png"));
        }
        clean(w);
    }
    void explicitProxyAndValidationDomains() {
        Window w;
        w.document().replace(physicsDemo("whole-body"));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QString originalId = w.document().id;
        auto original = sceneToJson(w.document().scene);
        EditorApi api(&w);
        auto preview = api.call("physics_analyze", {{"operation", "proxy_preview"}});
        QVERIFY2(!preview["isError"].toBool(), qPrintable(QJsonDocument(preview).toJson()));
        QCOMPARE(w.document().id, originalId);
        QCOMPARE(sceneToJson(w.document().scene), original);
        bool seen = false;
        QTimer::singleShot(150, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            QTimer::singleShot(10000, dialog, &QDialog::reject);
            seen = true;
            QCOMPARE(dialog->objectName(), QString("physicsProxyDialog"));
            QCOMPARE(dialog->findChildren<Viewport *>().size(), 2);
            auto fps = dialog->findChild<QComboBox *>("physicsProxyFps");
            QVERIFY(fps);
            fps->setCurrentIndex(1);
            QVERIFY(!dialog->findChild<QPushButton *>("openPhysicsProxyButton")->isEnabled());
            QTest::mouseClick(dialog->findChild<QPushButton *>("previewPhysicsProxyButton"), Qt::LeftButton);
            QVERIFY(dialog->findChild<QPushButton *>("openPhysicsProxyButton")->isEnabled());
            auto slider = dialog->findChild<QSlider *>("physicsProxyFrameSlider");
            QVERIFY(slider);
            slider->setValue(15);
            dialog->grab().save("artifacts/physics-ui/rigid-proxy-" + QString::number(w.devicePixelRatioF()) +
                                ".png");
            QTest::mouseClick(dialog->findChild<QPushButton *>("openPhysicsProxyButton"), Qt::LeftButton);
        });
        w.physicsProxyDialog();
        QVERIFY(seen);
        QCOMPARE(w.workspace.documents.size(), size_t(2));
        QVERIFY(w.document().scene.physics.requests["rigProxy"].toObject()["approximate"].toBool());
        QVERIFY(w.workspace.find(originalId));
        QCOMPARE(sceneToJson(w.workspace.find(originalId)->scene), original);
        // Both proxy entry points must update scene/camera bookkeeping before switching/closing.
        w.switchScene(0);
        w.switchScene(1);
        auto created = api.call("physics_analyze", {{"operation", "proxy_create"}});
        QVERIFY2(!created["isError"].toBool(), qPrintable(QJsonDocument(created).toJson()));
        QCOMPARE(w.workspace.documents.size(), size_t(3));
        w.switchScene(0);
        w.switchScene(2);
        clean(w);
    }
    void flightUiBackgroundAndStale() {
        Window w;
        w.document().replace(physicsDemo("jump"));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QString sourceId = w.document().id;
        QTimer::singleShot(80, [&] {
            auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            QVERIFY(dialog->findChild<QCheckBox *>("flightConserveAngular")->isChecked());
            QVERIFY(!dialog->findChild<QSpinBox *>("solverIterations")->isVisible());
            QVERIFY(!dialog->findChild<QCheckBox *>("physicsEndAtRest")->isVisible());
            QTest::mouseClick(dialog->findChild<QPushButton *>("calculatePhysicsButton"), Qt::LeftButton);
        });
        w.physicsCalculationDialog("flight");
        QTRY_VERIFY_WITH_TIMEOUT(w.hasPhysicsResult(), 10000);
        QVERIFY(w.physicsTaskState()["report"].toObject()["status"] == "solved");
        QCOMPARE(w.document().id, sourceId);
        auto preview = w.physicsResult();
        QVERIFY(preview->physicsResult.contains("inputSha256"));
        w.document().edit("Change source", [](Scene &s) { s.physics.gravity = 8; }, false);
        w.pollPhysicsTask();
        QVERIFY(!w.hasPhysicsResult());
        QCOMPARE(w.physicsTaskState()["status"].toString(), QString("stale"));
        clean(w);
    }
};
#include "atom_test_main.h"
int main(int argc,char** argv) { return runJesterTests<PhysicsUI>(argc,argv,"JesterPhysicsUiTests"); }
#include "physics_ui_tests.moc"
