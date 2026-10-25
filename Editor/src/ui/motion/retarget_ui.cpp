#include "core/scene/runtime_error.h"
#include "core/animation/retarget.h"
#include "ui/window/window.h"
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>
#include <QSaveFile>
#include <QScreen>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
namespace trickster {
void Window::showRetarget() {
    auto &targetDocument = document();
    if (targetDocument.inTransaction()) {
        report("Finish the current edit first.");
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("Humanoid retarget / Source - Target");
    dialog.resize(1060, 850);
    auto available = screen()->availableGeometry();
    available.setSize(available.size().boundedTo(size()));
    dialog.resize(std::min(dialog.width(), available.width() - 30),
                  std::min(dialog.height(), available.height() - 50));
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto source = new QComboBox;
    for (int i = 0; i < int(workspace.documents.size()); ++i)
        if (workspace.documents[i].get() != &targetDocument)
            source->addItem(workspace.documents[i]->scene.name, i);
    form->addRow("Source", source);
    form->addRow("Target", new QLabel(targetDocument.scene.name));
    auto root = new QComboBox;
    root->addItems({"scale", "preserve", "inPlace"});
    auto rate = new QSpinBox;
    rate->setRange(1, 240);
    rate->setValue(targetDocument.scene.fps);
    auto policies = new QHBoxLayout;
    policies->addWidget(root);
    policies->addWidget(new QLabel("FPS"));
    policies->addWidget(rate);
    auto ik = new QCheckBox("IK hands/feet");
    ik->setChecked(true);
    policies->addWidget(ik);
    auto contacts = new QCheckBox("Authored contacts");
    contacts->setChecked(true);
    policies->addWidget(contacts);
    form->addRow("Root translation (yaw preserved)", policies);
    auto basisRow = new QHBoxLayout;
    std::array<QDoubleSpinBox *, 3> basisAngles;
    for (int c = 0; c < 3; ++c) {
        auto edit = new QDoubleSpinBox;
        edit->setRange(-360, 360);
        edit->setDecimals(3);
        basisAngles[c] = edit;
        basisRow->addWidget(edit);
    }
    form->addRow("Source to target basis (YXZ degrees)", basisRow);
    auto tabs = new QTabWidget;
    layout->addWidget(tabs, 1);
    auto table = new QTableWidget;
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({"Role", "Source bone", "Target bone", "Orientation weight", "IK"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tabs->addTab(table, "Mapping");
    auto calibration = new QTableWidget;
    calibration->setColumnCount(7);
    calibration->setHorizontalHeaderLabels(
        {"Role", "Source X", "Source Y", "Source Z", "Target X", "Target Y", "Target Z"});
    calibration->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tabs->addTab(calibration, "Calibration (local YXZ degrees)");
    auto solePage = new QWidget;
    auto soleLayout = new QVBoxLayout(solePage);
    auto soleTable = new QTableWidget;
    soleTable->setColumnCount(7);
    soleTable->setHorizontalHeaderLabels(
        {"Foot role", "Source X", "Source Y", "Source Z", "Target X", "Target Y", "Target Z"});
    soleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    soleLayout->addWidget(soleTable, 1);
    auto adaptPelvis = new QCheckBox("Adjust pelvis for contact reach (bounded projection)");
    adaptPelvis->setChecked(true);
    soleLayout->addWidget(adaptPelvis);
    auto soleNote = new QLabel(
        "Local sole points use scene units. Defaults project the rest ankle onto the ground; review them "
        "against the actual foot mesh. Shared root/pelvis allows vertical reach adjustment only.");
    soleNote->setWordWrap(true);
    soleLayout->addWidget(soleNote);
    tabs->addTab(solePage, "Soles / contacts");
    auto previewWidget = new QWidget;
    auto previewLayout = new QVBoxLayout(previewWidget);
    auto split = new QSplitter;
    previewLayout->addWidget(split, 1);
    Document sourceViewDocument, targetViewDocument;
    auto sourceView = new Viewport(&sourceViewDocument);
    auto targetView = new Viewport(&targetViewDocument);
    sourceView->readOnly = targetView->readOnly = true;
    split->addWidget(sourceView);
    split->addWidget(targetView);
    auto frame = new QSlider(Qt::Horizontal);
    previewLayout->addWidget(frame);
    tabs->addTab(previewWidget, "Source / result preview");
    auto status = new QTextEdit;
    status->setReadOnly(true);
    status->setMaximumHeight(110);
    layout->addWidget(status);
    auto progress = new QProgressBar;
    progress->setRange(0, 100);
    layout->addWidget(progress);
    auto buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    auto suggest = new QPushButton("Suggest"), load = new QPushButton("Load profile"),
         save = new QPushButton("Save profile"), calculate = new QPushButton("Calculate preview"),
         cancel = new QPushButton("Cancel"), create = new QPushButton("Create result");
    for (auto b : {suggest, load, save, calculate, cancel, create})
        buttons->addWidget(b);
    create->setEnabled(false);
    QString jobId;
    QJsonObject calculatedSettings;
    QTimer poll;
    auto settings = [&] {
        QJsonArray mapping, sourceCalibration, targetCalibration;
        for (int row = 0; row < table->rowCount(); ++row) {
            auto a = qobject_cast<QComboBox *>(table->cellWidget(row, 1)),
                 b = qobject_cast<QComboBox *>(table->cellWidget(row, 2));
            if (a->currentIndex() < 0 || b->currentIndex() < 0)
                continue;
            mapping.append(QJsonObject{
                {"role", table->item(row, 0)->text()},
                {"source", a->currentIndex()},
                {"target", b->currentIndex()},
                {"orientationWeight", qobject_cast<QDoubleSpinBox *>(table->cellWidget(row, 3))->value()},
                {"ik", qobject_cast<QCheckBox *>(table->cellWidget(row, 4))->isChecked()}});
            QJsonArray sourceAngles, targetAngles;
            for (int axis = 0; axis < 3; ++axis) {
                sourceAngles.append(
                    qobject_cast<QDoubleSpinBox *>(calibration->cellWidget(row, axis + 1))->value());
                targetAngles.append(
                    qobject_cast<QDoubleSpinBox *>(calibration->cellWidget(row, axis + 4))->value());
            }
            sourceCalibration.append(QJsonObject{{"node", a->currentIndex()}, {"rotation", sourceAngles}});
            targetCalibration.append(QJsonObject{{"node", b->currentIndex()}, {"rotation", targetAngles}});
        }
        QJsonObject result{{"mapping", mapping},
                           {"sourceCalibration", sourceCalibration},
                           {"targetCalibration", targetCalibration},
                           {"fps", rate->value()},
                           {"ik", ik->isChecked()},
                           {"contacts", contacts->isChecked()},
                           {"rootMotion", root->currentText()}};
        QJsonArray sourceSoles, targetSoles;
        for (int row = 0; row < soleTable->rowCount(); ++row) {
            QString role = soleTable->item(row, 0)->text();
            for (auto entry : mapping) {
                auto pair = entry.toObject();
                if (pair["role"] != role)
                    continue;
                QJsonArray a, b;
                for (int c = 0; c < 3; ++c) {
                    a.append(qobject_cast<QDoubleSpinBox *>(soleTable->cellWidget(row, c + 1))->value());
                    b.append(qobject_cast<QDoubleSpinBox *>(soleTable->cellWidget(row, c + 4))->value());
                }
                sourceSoles.append(QJsonObject{{"node", pair["source"]}, {"point", a}});
                targetSoles.append(QJsonObject{{"node", pair["target"]}, {"point", b}});
            }
        }
        result["sourceSoles"] = sourceSoles;
        result["targetSoles"] = targetSoles;
        result["adaptPelvis"] = adaptPelvis->isChecked();
        result["basisEuler"] =
            QJsonArray{basisAngles[0]->value(), basisAngles[1]->value(), basisAngles[2]->value()};
        if (source->currentIndex() >= 0) {
            result["sourceFingerprint"] =
                retargetFingerprint(workspace.documents[source->currentData().toInt()]->scene);
            result["targetFingerprint"] = retargetFingerprint(targetDocument.scene);
        }
        return result;
    };
    auto populate = [&](QJsonObject profile) {
        if (source->currentIndex() < 0) {
            status->setText("Open a separate source document first.");
            return;
        }
        auto &s = workspace.documents[source->currentData().toInt()]->scene;
        auto &t = targetDocument.scene;
        auto pairs = profile["mapping"].toArray();
        table->setRowCount(0);
        calibration->setRowCount(0);
        for (auto value : pairs) {
            auto pair = value.toObject();
            int row = table->rowCount();
            table->insertRow(row);
            calibration->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(pair["role"].toString()));
            calibration->setItem(row, 0, new QTableWidgetItem(pair["role"].toString()));
            auto a = new QComboBox, b = new QComboBox;
            for (auto &n : s.nodes)
                a->addItem(n.name);
            for (auto &n : t.nodes)
                b->addItem(n.name);
            a->setCurrentIndex(pair["source"].toInt(-1));
            b->setCurrentIndex(pair["target"].toInt(-1));
            table->setCellWidget(row, 1, a);
            table->setCellWidget(row, 2, b);
            auto weight = new QDoubleSpinBox;
            weight->setRange(0, 1);
            weight->setSingleStep(.05);
            weight->setValue(pair["orientationWeight"].toDouble(1));
            table->setCellWidget(row, 3, weight);
            auto chainIk = new QCheckBox;
            chainIk->setChecked(pair["ik"].toBool(true));
            table->setCellWidget(row, 4, chainIk);
            for (int side = 0; side < 2; ++side)
                for (int axis = 0; axis < 3; ++axis) {
                    auto edit = new QDoubleSpinBox;
                    edit->setRange(-720, 720);
                    edit->setDecimals(5);
                    int n = side ? b->currentIndex() : a->currentIndex();
                    auto &scene = side ? t : s;
                    double angle = n >= 0 ? scene.nodes[n].rest.r.toEulerAngles()[axis] : 0;
                    for (auto value : profile[side ? "targetCalibration" : "sourceCalibration"].toArray()) {
                        auto item = value.toObject();
                        if (item["node"].toInt(-1) == n)
                            angle = item["rotation"].toArray()[axis].toDouble();
                    }
                    edit->setValue(angle);
                    calibration->setCellWidget(row, 1 + side * 3 + axis, edit);
                }
            // Changing a mapped bone resets its reference angles to that bone's rest pose.
            connect(a, &QComboBox::currentIndexChanged, &dialog, [&, row, a] {
                if (a->currentIndex() < 0)
                    return;
                auto angles = workspace.documents[source->currentData().toInt()]
                                  ->scene.nodes[a->currentIndex()]
                                  .rest.r.toEulerAngles();
                for (int c = 0; c < 3; ++c)
                    qobject_cast<QDoubleSpinBox *>(calibration->cellWidget(row, c + 1))->setValue(angles[c]);
            });
            connect(b, &QComboBox::currentIndexChanged, &dialog, [&, row, b] {
                if (b->currentIndex() < 0)
                    return;
                auto angles = targetDocument.scene.nodes[b->currentIndex()].rest.r.toEulerAngles();
                for (int c = 0; c < 3; ++c)
                    qobject_cast<QDoubleSpinBox *>(calibration->cellWidget(row, c + 4))->setValue(angles[c]);
            });
        }
        soleTable->setRowCount(0);
        auto sg = s.globals(s.restPose()), tg = t.globals(t.restPose());
        for (auto value : pairs) {
            auto pair = value.toObject();
            QString role = pair["role"].toString();
            if (role != "leftFoot" && role != "rightFoot")
                continue;
            int a = pair["source"].toInt(-1), b = pair["target"].toInt(-1);
            if (a < 0 || b < 0)
                continue;
            int row = soleTable->rowCount();
            soleTable->insertRow(row);
            soleTable->setItem(row, 0, new QTableWidgetItem(role));
            for (int side = 0; side < 2; ++side) {
                int n = side ? b : a;
                auto &scene = side ? t : s;
                auto &matrices = side ? tg : sg;
                auto origin = matrices[n].map(QVector3D());
                QVector3D local =
                    matrices[n].inverted().map(QVector3D(origin.x(), scene.physics.ground, origin.z()));
                for (auto entry : profile[side ? "targetSoles" : "sourceSoles"].toArray()) {
                    auto item = entry.toObject();
                    if (item["node"].toInt(-1) == n) {
                        auto p = item["point"].toArray();
                        if (p.size() == 3)
                            local = {float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble())};
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    auto edit = new QDoubleSpinBox;
                    edit->setDecimals(6);
                    edit->setRange(-1e6, 1e6);
                    edit->setValue(local[c]);
                    soleTable->setCellWidget(row, 1 + side * 3 + c, edit);
                }
            }
        }
        adaptPelvis->setChecked(profile["adaptPelvis"].toBool(true));
        auto basisValues = profile["basisEuler"].toArray();
        for (int c = 0; c < 3; ++c)
            basisAngles[c]->setValue(c < basisValues.size() ? basisValues[c].toDouble() : 0);
        if (profile.contains("rootMotion"))
            root->setCurrentText(profile["rootMotion"].toString());
        if (profile.contains("fps"))
            rate->setValue(profile["fps"].toInt());
        if (profile.contains("ik"))
            ik->setChecked(profile["ik"].toBool());
        if (profile.contains("contacts"))
            contacts->setChecked(profile["contacts"].toBool());
        status->setPlainText(QString::fromUtf8(QJsonDocument(profile).toJson()));
        create->setEnabled(false);
    };
    auto suggestMapping = [&] {
        if (source->currentIndex() < 0) {
            status->setText("Open a separate source document first.");
            return;
        }
        populate(
            suggestRetarget(workspace.documents[source->currentData().toInt()]->scene, targetDocument.scene));
    };
    connect(source, &QComboBox::currentIndexChanged, &dialog, [&] { suggestMapping(); });
    connect(suggest, &QPushButton::clicked, &dialog, suggestMapping);
    connect(calculate, &QPushButton::clicked, &dialog, [&] {
        try {
            if (source->currentIndex() < 0)
                throw trickster::RuntimeError("Open a source document first");
            calculatedSettings = settings();
            auto state = analysisJobs.start(targetDocument, "retarget", calculatedSettings,
                                            workspace.documents[source->currentData().toInt()].get());
            jobId = state["id"].toString();
            create->setEnabled(false);
            calculate->setEnabled(false);
            status->setText("Calculating immutable snapshots...");
            poll.start(50);
        } catch (const std::exception &e) {
            QMessageBox::warning(&dialog, "Retarget", e.what());
        }
    });
    connect(&poll, &QTimer::timeout, &dialog, [&] {
        analysisJobs.poll();
        auto state = analysisJobs.state(jobId);
        progress->setValue(state["progress"].toInt());
        if (state["status"] == "running")
            return;
        poll.stop();
        calculate->setEnabled(true);
        if (state["status"] != "ready") {
            status->setText(state["status"].toString() + ": " + state["error"].toString());
            return;
        }
        try {
            auto report = analysisJobs.result(jobId, 0, 1);
            status->setPlainText(
                QString("%1 / %2 keys / IK residual %3 mm / %4 unreachable samples\nSole error %5 mm; "
                        "maximum slip per sample %6 mm; sole penetration %7 mm.\nReview mapping, calibration "
                        "and sole points. Full verification/settings are retained in the result profile.")
                    .arg(report["status"].toString())
                    .arg(report["frames"].toInt())
                    .arg(report["maximumIkResidualSceneUnits"].toDouble() *
                             targetDocument.scene.physics.metersPerUnit * 1000,
                         0, 'g', 5)
                    .arg(report["unreachableSamples"].toInt())
                    .arg(report["maximumFootGoalResidualSceneUnits"].toDouble() *
                             targetDocument.scene.physics.metersPerUnit * 1000,
                         0, 'g', 5)
                    .arg(report["maximumTangentialSlipPerSampleMetres"].toDouble() * 1000, 0, 'g', 5)
                    .arg(report["maximumSolePointPenetrationMetres"].toDouble() * 1000, 0, 'g', 5));
            sourceViewDocument.replace(workspace.documents[source->currentData().toInt()]->scene);
            targetViewDocument.replace(analysisJobs.resultScene(jobId, targetDocument));
            sourceView->fit();
            targetView->fit();
            frame->setRange(sourceViewDocument.scene.first, sourceViewDocument.scene.last);
            frame->setValue(sourceViewDocument.scene.first);
            sourceViewDocument.seek(sourceViewDocument.scene.first);
            targetViewDocument.seek(qRound(double(sourceViewDocument.scene.first) /
                                           sourceViewDocument.scene.fps * targetViewDocument.scene.fps));
            create->setEnabled(settings() == calculatedSettings);
            tabs->setCurrentWidget(previewWidget);
        } catch (const std::exception &e) {
            status->setText(e.what());
        }
    });
    connect(frame, &QSlider::valueChanged, &dialog, [&](int value) {
        sourceViewDocument.seek(value);
        targetViewDocument.seek(
            qRound(double(value) * targetViewDocument.scene.fps / sourceViewDocument.scene.fps));
    });
    connect(cancel, &QPushButton::clicked, &dialog, [&] {
        if (!jobId.isEmpty())
            analysisJobs.cancel(jobId);
    });
    connect(create, &QPushButton::clicked, &dialog, [&] {
        try {
            if (settings() != calculatedSettings)
                throw trickster::RuntimeError("Settings changed. Calculate the preview again.");
            auto result = analysisJobs.resultScene(jobId, targetDocument);
            dialog.accept();
            workspace.append(std::move(result), {}, false);
        } catch (const std::exception &e) {
            QMessageBox::warning(&dialog, "Retarget", e.what());
        }
    });
    connect(save, &QPushButton::clicked, &dialog, [&] {
        auto path = QFileDialog::getSaveFileName(&dialog, "Save retarget profile", {}, "JSON (*.json)");
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            status->setText(file.errorString());
            return;
        }
        file.write(QJsonDocument(settings()).toJson());
        if (!file.commit())
            status->setText(file.errorString());
    });
    connect(load, &QPushButton::clicked, &dialog, [&] {
        auto path = QFileDialog::getOpenFileName(&dialog, "Load retarget profile", {}, "JSON (*.json)");
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            status->setText(file.errorString());
            return;
        }
        QJsonParseError error;
        auto object = QJsonDocument::fromJson(file.readAll(), &error).object();
        if (error.error != QJsonParseError::NoError || source->currentIndex() < 0 ||
            object["sourceFingerprint"] !=
                retargetFingerprint(workspace.documents[source->currentData().toInt()]->scene) ||
            object["targetFingerprint"] != retargetFingerprint(targetDocument.scene)) {
            status->setText("Invalid JSON or profile skeleton fingerprints differ. Review a new mapping.");
            return;
        }
        populate(object);
    });
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QWidget::customContextMenuRequested, &dialog, [&](QPoint point) {
        QMenu menu;
        auto add = menu.addAction("Add custom bone mapping"), remove = menu.addAction("Remove selected role");
        auto chosen = menu.exec(table->viewport()->mapToGlobal(point));
        if (chosen == add) {
            auto profile = settings();
            auto pairs = profile["mapping"].toArray();
            int s = source->currentIndex() >= 0 ? workspace.documents[source->currentData().toInt()]->selected
                                                : -1,
                t = targetDocument.selected;
            pairs.append(QJsonObject{
                {"role", QString("custom%1").arg(table->rowCount() + 1)}, {"source", s}, {"target", t}});
            profile["mapping"] = pairs;
            populate(profile);
        } else if (chosen == remove && table->currentRow() >= 0) {
            auto profile = settings();
            auto pairs = profile["mapping"].toArray();
            QString role = table->item(table->currentRow(), 0)->text();
            for (int row = pairs.size() - 1; row >= 0; --row)
                if (pairs[row].toObject()["role"] == role)
                    pairs.removeAt(row);
            profile["mapping"] = pairs;
            populate(profile);
        }
    });
    suggestMapping();
    dialog.exec();
    if (poll.isActive() && !jobId.isEmpty())
        analysisJobs.cancel(jobId);
    for (auto object : dialog.findChildren<QObject *>())
        QObject::disconnect(object, nullptr, &dialog, nullptr);
    delete sourceView;
    delete targetView;
}
} // namespace trickster
