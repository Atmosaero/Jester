#include "ui/widgets/localized_ui.h"
#include "core/scene/edit_service.h"
#include "ui/widgets/icons.h"
#include "physics/physics_proxy.h"
#include "physics/physics_tools.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace trickster {
namespace {
QJsonArray vec(QVector3D v) {
    return {v.x(), v.y(), v.z()};
}
QDoubleSpinBox *number(QWidget *parent, const QString &name, double value, double lo = -10000,
                       double hi = 10000) {
    auto b = new QDoubleSpinBox(parent);
    b->setObjectName(name);
    b->setRange(lo, hi);
    b->setDecimals(5);
    b->setValue(value);
    b->setKeyboardTracking(false);
    return b;
}
QWidget *vectorEditor(QWidget *parent, const QString &name, QVector3D value) {
    auto w = new QWidget(parent);
    auto l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    for (int k = 0; k < 3; ++k) {
        auto box = number(w, name + QString::number(k), value[k]);
        box->setPrefix(QString("XYZ")[k] + uiText(" "));
        l->addWidget(box);
    }
    return w;
}
QJsonArray vectorValue(QWidget *w) {
    QJsonArray a;
    for (auto b : w->findChildren<QDoubleSpinBox *>())
        a.append(b->value());
    return a;
}
QString reportText(const QJsonObject &r) {
    return uiText("%1 · %2 ms\nSupport %3 mm · target %4 mm · COM %5 mm\n%6\nStatic pose check; dynamic "
                   "validation is separate.")
        .arg(r["status"].toString())
        .arg(r["elapsedMs"].toDouble(), 0, 'f', 0)
        .arg(r["contactErrorM"].toDouble() * 1000, 0, 'f', 2)
        .arg(r["targetErrorM"].toDouble() * 1000, 0, 'f', 2)
        .arg(r["comErrorM"].toDouble() * 1000, 0, 'f', 2)
        .arg(r["stopReason"].toString());
}
} // namespace
void Window::showWholeBodyDialog() {
    commitPendingEditor();
    document().play(false);
    if (document().scene.nodes.empty()) {
        report(QString("Open a character first."));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("wholeBodyDialog");
    bindUiText(&dialog,"windowTitle","Whole-body pose · Jester");
    dialog.resize(std::clamp(width()-80,700,900),430);
    auto layout = new QVBoxLayout(&dialog);
    auto goals = new QTableWidget(0, 8, &dialog);
    goals->setObjectName("wholeBodyGoals");
    bindHeaders(goals,{"Effector", "Hard", "Priority", "Weight", "X", "Y", "Z", ""});
    goals->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    goals->horizontalHeader()->setStretchLastSection(false);
    goals->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Fixed);
    goals->setColumnWidth(7, 28);
    for(int column=4;column<=6;++column)goals->horizontalHeader()->setSectionResizeMode(column,QHeaderView::Stretch);
    auto &s = document().scene;
    auto defs = s.bodies.empty() ? fittedPhysicsRig(s) : s.bodies;
    if (defs.empty()) {
        report(QString("No physical bodies. Fit or add bodies in Model before posing."));
        return;
    }
    auto defaults = s.physics.requests["whole_body"].toObject();
    auto add = [&](int node, bool hard) {
        int row = goals->rowCount();
        goals->insertRow(row);
        auto combo = new QComboBox(goals);
        for (const auto &body : defs)
            combo->addItem(s.nodes[body.node].name, body.node);
        combo->setCurrentIndex(combo->findData(node));
        goals->setCellWidget(row, 0, combo);
        auto check = new QCheckBox(goals);
        check->setChecked(hard);
        goals->setCellWidget(row, 1, check);
        auto priority = new QSpinBox(goals);
        priority->setRange(0, 3);
        priority->setValue(hard ? 3 : 0);
        goals->setCellWidget(row, 2, priority);
        goals->setCellWidget(row, 3, number(goals, "goalWeight", 1, .001, 10000));
        auto current = document().scene.positions(document().scene.pose);
        for (int k = 0; k < 3; ++k)
            goals->setCellWidget(row, 4 + k,
                                 number(goals, "goalPosition" + QString::number(k), current[node][k]));
        auto rowOf = [goals, combo] {
            for (int r = 0; r < goals->rowCount(); ++r)
                if (goals->cellWidget(r, 0) == combo)
                    return r;
            return -1;
        };
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), goals, [=, this] {
            int r = rowOf(), n = combo->currentData().toInt();
            if (r < 0)
                return;
            combo->setProperty("goalOrientation", QVariant());
            auto current = document().scene.positions(document().scene.pose);
            for (int k = 0; k < 3; ++k)
                qobject_cast<QDoubleSpinBox *>(goals->cellWidget(r, 4 + k))->setValue(current[n][k]);
        });
        auto remove = new QToolButton(goals);
        remove->setText(QString(QChar(0x00d7)));
        remove->setObjectName("removeWholeBodyGoal");
        bindUiText(remove,"toolTip","Remove this additional target; authored contacts stay in the Contacts editor.");
        bindUiText(remove,"accessibleName","Remove target");
        goals->setCellWidget(row, 7, remove);
        connect(remove, &QToolButton::clicked, goals, [=] {
            int r = rowOf();
            if (r >= 0)
                goals->removeRow(r);
        });
    };
    for (const auto &body : defs)
        if (s.isFoot(body.node) && s.isEffector(body.node))
            add(body.node, true);
    int selected = document().selected;
    if (selected < 0 || std::none_of(defs.begin(), defs.end(), [&](auto b) { return b.node == selected; })) {
        selected = defs.front().node;
        for (const auto &body : defs)
            if (s.isEffector(body.node) && !s.isFoot(body.node)) {
                selected = body.node;
                break;
            }
    }
    if (!defaults.contains("comTarget")) {
        bool present = false;
        for (int r = 0; r < goals->rowCount(); ++r)
            if (qobject_cast<QComboBox *>(goals->cellWidget(r, 0))->currentData().toInt() == selected)
                present = true;
        if (!present)
            add(selected, false);
    }
    if (defaults.contains("useContacts"))
        goals->setRowCount(0);
    bool missingGoal = false;
    for (auto value : defaults["goals"].toArray()) {
        auto g = value.toObject();
        int n = g["node"].toInt(-1);
        if (g["node"].isString())
            for (int id = 0; id < int(s.nodes.size()); ++id)
                if (s.nodes[id].name == g["node"].toString()) {
                    n = id;
                    break;
                }
        if (std::none_of(defs.begin(), defs.end(), [&](const auto &body) { return body.node == n; })) {
            missingGoal = true;
            continue;
        }
        int row = -1;
        if (!defaults.contains("useContacts"))
            for (int r = 0; r < goals->rowCount(); ++r)
                if (qobject_cast<QComboBox *>(goals->cellWidget(r, 0))->currentData().toInt() == n) {
                    row = r;
                    break;
                }
        if (row < 0) {
            add(n, g["hard"].toBool());
            row = goals->rowCount() - 1;
        }
        qobject_cast<QCheckBox *>(goals->cellWidget(row, 1))->setChecked(g["hard"].toBool());
        qobject_cast<QSpinBox *>(goals->cellWidget(row, 2))
            ->setValue(g["priority"].toInt(g["hard"].toBool() ? 3 : 0));
        auto weight = qobject_cast<QDoubleSpinBox *>(goals->cellWidget(row, 3));
        double requested = g["weight"].toDouble(1);
        if (std::isfinite(requested) && requested > 0) {
            weight->setRange(std::min(.001, requested), std::max(10000., requested));
            weight->setValue(requested);
        }
        auto target = g["target"].toArray();
        if (target.size() == 3)
            for (int k = 0; k < 3; ++k)
                qobject_cast<QDoubleSpinBox *>(goals->cellWidget(row, 4 + k))->setValue(target[k].toDouble());
        else
            missingGoal = true;
        if (g["orientation"].toArray().size() == 3)
            goals->cellWidget(row, 0)->setProperty("goalOrientation",
                                                   g["orientation"].toArray().toVariantList());
    }
    layout->addWidget(goals);
    auto addGoal = translatedIconWidget<QPushButton>(icon(Glyph::Pin),"Add target", &dialog);
    addGoal->setObjectName("addWholeBodyGoal");
    connect(addGoal, &QPushButton::clicked, &dialog, [&] { add(selected, false); });
    layout->addWidget(addGoal);
    auto form = new QFormLayout;
    auto comEnabled = translatedWidget<QCheckBox>("COM goal", &dialog);
    comEnabled->setObjectName("wholeBodyComEnabled");
    auto comTarget = physicalCenterOfMass(s);
    auto requestedCom = defaults["comTarget"].toArray();
    if (requestedCom.size() == 3) {
        comTarget = {float(requestedCom[0].toDouble()), float(requestedCom[1].toDouble()),
                     float(requestedCom[2].toDouble())};
        comEnabled->setChecked(true);
    }
    auto com = vectorEditor(&dialog, "wholeBodyCom", comTarget);
    auto comHeight = translatedWidget<QCheckBox>("Height", &dialog);
    comHeight->setObjectName("wholeBodyComHeight");
    comHeight->setChecked(defaults["comHeight"].toBool());
    auto comControls = new QWidget(&dialog);
    auto comRow = new QHBoxLayout(comControls);
    comRow->setContentsMargins(0, 0, 0, 0);
    comRow->addWidget(comEnabled);
    comRow->addWidget(comHeight);
    auto updateCom = [=] {
        com->setEnabled(comEnabled->isChecked());
        comHeight->setEnabled(comEnabled->isChecked());
        com->findChildren<QDoubleSpinBox *>()[1]->setEnabled(comHeight->isChecked());
    };
    connect(comEnabled, &QCheckBox::toggled, &dialog, updateCom);
    connect(comHeight, &QCheckBox::toggled, &dialog, updateCom);
    updateCom();
    form->addRow(comControls, com);
    layout->addLayout(form);
    auto stats = new QLabel(
        missingGoal
            ? uiText("Some saved targets no longer have a physical body; review goals.")
            : uiText("Hard targets preserve supports. COM is a static hint; authored contacts remain active."),
        &dialog);
    stats->setObjectName("wholeBodyReport");
    stats->setMinimumHeight(stats->fontMetrics().lineSpacing() * 6 + 8);
    stats->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    stats->setWordWrap(true);
    layout->addWidget(stats);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto solve = addTranslatedButton(buttons,"Solve + key", QDialogButtonBox::ActionRole);
    solve->setObjectName("solveWholeBodyButton");
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(solve, &QPushButton::clicked, &dialog, [&] {
        try {
            QJsonArray targets;
            for (int row = 0; row < goals->rowCount(); ++row) {
                QJsonArray target;
                for (int k = 0; k < 3; ++k)
                    target.append(qobject_cast<QDoubleSpinBox *>(goals->cellWidget(row, 4 + k))->value());
                auto combo = qobject_cast<QComboBox *>(goals->cellWidget(row, 0));
                QJsonObject targetGoal{
                    {"node", combo->currentData().toInt()},
                    {"hard", qobject_cast<QCheckBox *>(goals->cellWidget(row, 1))->isChecked()},
                    {"priority", qobject_cast<QSpinBox *>(goals->cellWidget(row, 2))->value()},
                    {"weight", qobject_cast<QDoubleSpinBox *>(goals->cellWidget(row, 3))->value()},
                    {"target", target}};
                if (combo->property("goalOrientation").isValid())
                    targetGoal["orientation"] =
                        QJsonArray::fromVariantList(combo->property("goalOrientation").toList());
                targets.append(targetGoal);
            }
            for (int row = 0; row < targets.size(); ++row) {
                auto target = targets[row].toObject();
                int n = target["node"].toInt();
                if (!target.contains("orientation") && target["hard"].toBool() && document().scene.isFoot(n))
                    target["orientation"] =
                        vec(document().scene.globalRotation(n, document().scene.pose).toEulerAngles());
                targets[row] = target;
            }
            QJsonObject args = defaults;
            args["goals"] = targets;
            args["useContacts"] = defaults["useContacts"].toBool(true);
            args["comHeight"] = comHeight->isChecked();
            if (comEnabled->isChecked())
                args["comTarget"] = vectorValue(com);
            else
                args.remove("comTarget");
            auto r = EditService::execute(document(), "whole_body", args, "Whole-body pose");
            stats->setText(reportText(r));
            viewport->showBalance = comEnabled->isChecked();
            refresh();
        } catch (const std::exception &e) {
            stats->setText(translatedDiagnostic(QString::fromUtf8(e.what())));
        }
    });
    layout->addWidget(buttons);
    dialog.exec();
}
void Window::physicsCalculationDialog(const QString &kind) {
    commitPendingEditor();
    document().play(false);
    QDialog dialog(this);
    dialog.setObjectName("physicsCalculationDialog");
    dialog.setWindowTitle(kind == "flight" ? uiText("Flight correction · Jester")
                                           : uiText("Dynamics correction · Jester"));
    dialog.resize(560, 430);
    auto l = new QVBoxLayout(&dialog);
    auto defaults = document().scene.physics.requests[kind].toObject();
    auto form = new QFormLayout;
    auto first = new QSpinBox(&dialog), last = new QSpinBox(&dialog);
    first->setObjectName("solverFirst");
    last->setObjectName("solverLast");
    first->setRange(document().scene.first, document().scene.last);
    last->setRange(document().scene.first, document().scene.last);
    first->setValue(physicsFrom->value());
    last->setValue(physicsTo->value());
    addTranslatedRow(form,"In", first);
    addTranslatedRow(form,"Out", last);
    auto mode = new QComboBox(&dialog);
    mode->setObjectName("flightBoundaryMode");
    addTranslatedItems(mode,{"Endpoint positions", "Initial position + velocity"});
    mode->setCurrentIndex(defaults["mode"] == "initial-velocity" ? 1 : 0);
    auto velocity = vectorEditor(&dialog, "flightInitialVelocity", {}),
         endVelocity = vectorEditor(&dialog, "flightEndVelocity", {});
    auto constrain = translatedWidget<QCheckBox>("Require end velocity (m/s)", &dialog);
    constrain->setObjectName("flightConstrainEndVelocity");
    auto angular = translatedWidget<QCheckBox>("Conserve angular momentum", &dialog);
    angular->setObjectName("flightConserveAngular");
    angular->setChecked(defaults["conserveAngularMomentum"].toBool(true));
    constrain->setChecked(defaults["constrainEndVelocity"].toBool());
    auto updateVelocityInputs = [=] {
        velocity->setEnabled(mode->currentIndex() == 1);
        endVelocity->setEnabled(constrain->isChecked());
    };
    connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, updateVelocityInputs);
    connect(constrain, &QCheckBox::toggled, &dialog, updateVelocityInputs);
    bindUiText(velocity,"toolTip","Endpoint mode derives velocity from positions and duration.");
    updateVelocityInputs();
    for (auto pair : {std::make_pair(velocity, QString("initialVelocity")),
                      std::make_pair(endVelocity, QString("endVelocity"))}) {
        auto values = defaults[pair.second].toArray();
        auto boxes = pair.first->findChildren<QDoubleSpinBox *>();
        if (values.size() == 3)
            for (int k = 0; k < 3; ++k)
                boxes[k]->setValue(values[k].toDouble());
    }
    auto iterations = new QSpinBox(&dialog);
    iterations->setObjectName("solverIterations");
    iterations->setRange(1, 200);
    iterations->setValue(defaults["iterations"].toInt(24));
    auto seconds = number(&dialog, "solverSeconds", defaults["seconds"].toDouble(15), .01, 300);
    auto important = new QLineEdit(&dialog);
    important->setObjectName("solverImportantFrames");
    bindUiText(important,"placeholderText","e.g. 0:10, 15:5, 30:10");
    QStringList frames;
    for (auto f : defaults["importantFrames"].toArray())
        frames << uiText("%1:%2").arg(f.toObject()["frame"].toInt()).arg(f.toObject()["weight"].toDouble(5));
    important->setText(frames.join(", "));
    auto endRest = translatedWidget<QCheckBox>("Rest at end", &dialog);
    endRest->setObjectName("physicsEndAtRest");
    endRest->setChecked(defaults["endAtRest"].toBool());
    auto prepareLanding = translatedWidget<QCheckBox>("Prepare landing reference", &dialog);
    prepareLanding->setObjectName("physicsPrepareLanding");
    prepareLanding->setChecked(defaults["prepareLanding"].toBool(true));
    bindUiText(prepareLanding,"toolTip","Adjust intermediate leg reference poses around authored contact. Important "
                               "poses, initial state and physical model remain fixed.");
    for (QWidget *widget : std::initializer_list<QWidget *>{mode, velocity, endVelocity, constrain, angular})
        widget->setVisible(kind == "flight");
    for (QWidget *widget :
         std::initializer_list<QWidget *>{iterations, seconds, important, endRest, prepareLanding})
        widget->setVisible(kind != "flight");
    if (kind == "flight") {
        addTranslatedRow(form,"Boundary", mode);
        addTranslatedRow(form,"Initial velocity (m/s)", velocity);
        form->addRow(constrain, endVelocity);
        form->addRow(angular);
    } else {
        addTranslatedRow(form,"Iterations", iterations);
        addTranslatedRow(form,"Budget (s)", seconds);
        addTranslatedRow(form,"Poses (frame:weight)", important);
        form->addRow(endRest);
        form->addRow(prepareLanding);
    }
    auto outputFps = new QSpinBox(&dialog);
    outputFps->setObjectName("physicsOutputFps");
    outputFps->setRange(0, 240);
    bindUiText(outputFps,"specialValueText","Auto (simulation detail)");
    outputFps->setValue(defaults["outputFps"].toInt());
    addTranslatedRow(form,"Result FPS", outputFps);
    l->addLayout(form);
    auto note =
        new QLabel(kind == "flight"
                       ? uiText("Release world pins; contact-free interval only. Incompatible velocity conditions "
                         "produce an error.")
                       : uiText("Contacts use real ground collision and the authored schedule. No root actuator or "
                         "hidden world anchor. An approximate result may miss goals; inspect the report."),
                   &dialog);
    note->setWordWrap(true);
    l->addWidget(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(uiText("Calculate"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName("calculatePhysicsButton");
    l->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QJsonObject settings;
        if (kind == "flight")
            settings = {{"mode", mode->currentIndex() == 0 ? "endpoint-positions" : "initial-velocity"},
                        {"initialVelocity", vectorValue(velocity)},
                        {"endVelocity", vectorValue(endVelocity)},
                        {"constrainEndVelocity", constrain->isChecked()},
                        {"conserveAngularMomentum", angular->isChecked()}};
        else {
            QJsonArray frames;
            for (const auto &text : important->text().split(',', Qt::SkipEmptyParts)) {
                auto parts = text.trimmed().split(':');
                bool ok = false, weightOk = true;
                int f = parts[0].trimmed().toInt(&ok);
                double weight = parts.size() == 2 ? parts[1].trimmed().toDouble(&weightOk) : 5;
                if (!ok || !weightOk || parts.size() > 2 || !std::isfinite(weight) || weight <= 0 ||
                    f < first->value() || f > last->value()) {
                    bindUiText(note,"text","Use frame or frame:weight, separated by commas. Frames must be inside the "
                                  "interval and weights positive.");
                    return;
                }
                frames.append(QJsonObject{{"frame", f}, {"weight", weight}});
            }
            settings = {{"iterations", iterations->value()},
                        {"seconds", seconds->value()},
                        {"importantFrames", frames},
                        {"endAtRest", endRest->isChecked()},
                        {"prepareLanding", prepareLanding->isChecked()}};
        }
        QJsonObject merged = defaults;
        for (auto it = settings.begin(); it != settings.end(); ++it)
            merged[it.key()] = it.value();
        settings = merged;
        settings["outputFps"] = outputFps->value();
        QString error;
        if (startPhysicsTask(kind, first->value(), last->value(), settings, error))
            dialog.accept();
        else
            note->setText(error);
    });
    dialog.exec();
}
void Window::physicsModelDialog() {
    commitPendingEditor();
    document().play(false);
    auto &s = document().scene;
    QDialog dialog(this);
    dialog.setObjectName("physicsModelDialog");
    bindUiText(&dialog,"windowTitle","Physical model · Jester");
    dialog.resize(760, 440);
    auto l = new QVBoxLayout(&dialog);
    auto summary = new QLabel(&dialog);
    summary->setWordWrap(true);
    l->addWidget(summary);
    auto measure = [&] {
        QString error;
        PhysicsMeasurement m;
        if (measurePhysics(document().scene, document().frame, m, error))
            summary->setText(uiText("%1 · %2 kg · COM (%3, %4, %5) m\nEstimated capsules/inertias are "
                                     "initial guesses; authored values are authoritative.")
                                 .arg(physicsBackendVersion())
                                 .arg(m.mass, 0, 'f', 3)
                                 .arg(m.com.x(), 0, 'f', 3)
                                 .arg(m.com.y(), 0, 'f', 3)
                                 .arg(m.com.z(), 0, 'f', 3));
        else
            summary->setText(error);
    };
    measure();
    auto defs = s.bodies.empty() ? fittedPhysicsRig(s) : s.bodies;
    auto table = new QTableWidget(int(defs.size()), 22, &dialog);
    table->setObjectName("physicsModelTable");
    bindHeaders(table,{"Body",       "kg",        "Radius u",  "COM X",      "COM Y",
                                      "COM Z",      "Ixx kgm²",  "Iyy kgm²",  "Izz kgm²",   "Foot X u",
                                      "Foot Y u",   "Foot Z u",  "Kp Nm/rad", "Kd Nms/rad", "Limit Nm",
                                      "Estimated",  "Shape Rx",  "Shape Ry",  "Shape Rz",   "Inertia Rx",
                                      "Inertia Ry", "Inertia Rz"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    auto rigRow = new QHBoxLayout;
    auto newBody = new QComboBox(&dialog);
    newBody->setObjectName("physicsNewBodyNode");
    for (int n = 0; n < int(s.nodes.size()); ++n)
        newBody->addItem(s.nodes[n].name, n);
    auto addBody = translatedWidget<QPushButton>("Add", &dialog), removeBody = translatedWidget<QPushButton>("Remove", &dialog);
    addBody->setObjectName("physicsAddBody");
    removeBody->setObjectName("physicsRemoveBody");
    rigRow->addWidget(newBody, 1);
    rigRow->addWidget(addBody);
    rigRow->addWidget(removeBody);
    l->addLayout(rigRow);
    auto populate = [&] {
        table->setRowCount(0);
        table->setRowCount(int(defs.size()));
        for (int r = 0; r < int(defs.size()); ++r) {
            const auto &b = defs[r];
            table->setItem(r, 0, new QTableWidgetItem(s.nodes[b.node].name));
            table->item(r, 0)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            double values[] = {b.mass,
                               b.radius,
                               b.center.x(),
                               b.center.y(),
                               b.center.z(),
                               b.inertia.x(),
                               b.inertia.y(),
                               b.inertia.z(),
                               b.footHalfSize.x(),
                               b.footHalfSize.y(),
                               b.footHalfSize.z(),
                               b.stiffness,
                               b.damping,
                               b.maxTorque};
            for (int c = 1; c <= 14; ++c) {
                auto box = number(table, "modelValue", values[c - 1],
                                  c <= 2 ? .000001 : (c <= 5 ? -10000 : (c <= 11 ? 0 : -1)), 100000);
                if (c >= 6 && c <= 8) {
                    box->setDecimals(12);
                    box->setValue(values[c - 1]);
                }
                box->setProperty("originalPhysicsValue", values[c - 1]);
                box->setProperty("physicsValueEdited", false);
                connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), box,
                        [box] { box->setProperty("physicsValueEdited", true); });
                table->setCellWidget(r, c, box);
            }
            auto check = new QCheckBox(table);
            check->setChecked(b.estimated);
            table->setCellWidget(r, 15, check);
            auto shape = b.shapeRotation.toEulerAngles(), inertia = b.inertiaRotation.toEulerAngles();
            for (int k = 0; k < 3; ++k) {
                table->setCellWidget(r, 16 + k, number(table, "shapeRotation", shape[k], -180, 180));
                table->setCellWidget(r, 19 + k, number(table, "inertiaRotation", inertia[k], -180, 180));
            }
        }
    };
    populate();
    connect(addBody, &QPushButton::clicked, &dialog, [&] {
        if (applyOperation("physics_body", {{"operation", "add"}, {"node", newBody->currentData().toInt()}},
                           "Add physical segment")) {
            defs = document().scene.bodies;
            populate();
            measure();
        }
    });
    connect(removeBody, &QPushButton::clicked, &dialog, [&] {
        int row = table->currentRow();
        if (row >= 0 && row < int(defs.size()) &&
            applyOperation("physics_body", {{"operation", "remove"}, {"node", defs[row].node}},
                           "Remove physical segment")) {
            defs = document().scene.bodies;
            populate();
            measure();
        }
    });
    l->addWidget(table);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Apply)->setObjectName("applyPhysicsModel");
    auto exportButton = addTranslatedButton(buttons,"Export MJCF", QDialogButtonBox::ActionRole);
    exportButton->setObjectName("exportMjcfButton");
    auto human = addTranslatedButton(buttons,"Fit human rig", QDialogButtonBox::ActionRole);
    human->setObjectName("fitHumanPhysicsRigButton");
    bindUiText(human,"toolTip","Replace the rig with estimated major-segment masses and capsules. Undo is available.");
    auto feet = addTranslatedButton(buttons,"Fit feet", QDialogButtonBox::ActionRole);
    feet->setObjectName("fitFootPadsButton");
    bindUiText(feet,"toolTip","Fit estimated oriented support boxes to the authored rest pose and ground.");
    connect(human, &QPushButton::clicked, &dialog, [&] {
        if (applyOperation("physics_body",
                           {{"operation", "fit"},
                            {"massProfile", "human"},
                            {"totalMass", physicalMass(document().scene)}},
                           "Fit human estimate")) {
            defs = document().scene.bodies;
            populate();
            measure();
        }
    });
    connect(feet, &QPushButton::clicked, &dialog, [&] {
        if (applyOperation("physics_body", {{"operation", "foot_pads"}}, "Fit foot supports")) {
            defs = document().scene.bodies;
            populate();
            measure();
        }
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        QJsonArray bodies;
        for (int r = 0; r < int(defs.size()); ++r) {
            auto value = [&](int c) {
                auto box = qobject_cast<QDoubleSpinBox *>(table->cellWidget(r, c));
                return box->property("originalPhysicsValue").isValid() &&
                               !box->property("physicsValueEdited").toBool()
                           ? box->property("originalPhysicsValue").toDouble()
                           : box->value();
            };
            auto shape = QQuaternion::fromEulerAngles(float(value(16)), float(value(17)), float(value(18))),
                 inertia = QQuaternion::fromEulerAngles(float(value(19)), float(value(20)), float(value(21)));
            bodies.append(QJsonObject{
                {"node", defs[r].node},
                {"shapeRotation", QJsonArray{shape.x(), shape.y(), shape.z(), shape.scalar()}},
                {"inertiaRotation", QJsonArray{inertia.x(), inertia.y(), inertia.z(), inertia.scalar()}},
                {"mass", value(1)},
                {"radius", value(2)},
                {"center", QJsonArray{value(3), value(4), value(5)}},
                {"inertia", QJsonArray{value(6), value(7), value(8)}},
                {"footHalfSize", QJsonArray{value(9), value(10), value(11)}},
                {"stiffness", value(12)},
                {"damping", value(13)},
                {"maxTorque", value(14)},
                {"estimated", qobject_cast<QCheckBox *>(table->cellWidget(r, 15))->isChecked()}});
        }
        if (applyOperation("physics_body", {{"operation", "set_many"}, {"bodies", bodies}}, "Physical model"))
            measure();
        else
            bindUiText(summary,"text","Invalid model: inertia must be positive and satisfy triangle inequalities; "
                             "foot half sizes must all be positive or all zero.");
    });
    connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        QString xml, error;
        if (!physicsModelXml(document().scene, xml, error, false)) {
            summary->setText(error);
            return;
        }
        auto path = QFileDialog::getSaveFileName(&dialog, uiText("Export model"), {}, uiText("MuJoCo model (*.xml)"));
        if (path.isEmpty())
            return;
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(xml.toUtf8()) < 0 || !f.commit())
            summary->setText(f.errorString());
        else
            summary->setText(uiText("MJCF saved: ") + path);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    l->addWidget(buttons);
    dialog.exec();
}
void Window::physicsProxyDialog() {
    commitPendingEditor();
    document().play(false);
    if (document().scene.nodes.empty()) {
        report(QString("Open a character before preparing a physical proxy."), true);
        return;
    }
    const Scene source = document().scene;
    const int sourceFrame = document().frame;
    const int sourceSelection = document().selected;
    const QString sourceId = document().id;
    const quint64 sourceRevision = document().motionRevision;
    Scene proxy;
    QJsonObject measurement;
    Document sourceView, proxyView;
    sourceView.replace(source);
    sourceView.seek(sourceFrame);
    sourceView.select(sourceSelection);
    QDialog dialog(this);
    dialog.setObjectName("physicsProxyDialog");
    bindUiText(&dialog,"windowTitle","Rigid proxy preview - Jester");
    dialog.resize(qMin(860, qRound(1200 / devicePixelRatioF())),
                  qMin(480, qRound(640 / devicePixelRatioF())));
    auto layout = new QVBoxLayout(&dialog);
    auto info = new QLabel(&dialog);
    info->setWordWrap(true);
    layout->addWidget(info);
    auto controls = new QHBoxLayout;
    layout->addLayout(controls);
    controls->addWidget(translatedWidget<QLabel>("Proxy FPS", &dialog));
    auto fps = new QComboBox(&dialog);
    fps->setObjectName("physicsProxyFps");
    for (int rate = source.fps; rate <= 240; rate += source.fps)
        fps->addItem(QString::number(rate), rate);
    fps->setCurrentIndex(qMax(0, fps->findData(QSettings().value("physics/proxyFps", source.fps).toInt())));
    controls->addWidget(fps);
    auto calculate = translatedWidget<QPushButton>("Preview", &dialog);
    calculate->setObjectName("previewPhysicsProxyButton");
    controls->addWidget(calculate);
    controls->addStretch();
    auto views = new QHBoxLayout;
    layout->addLayout(views, 1);
    auto originalViewport = new Viewport(&sourceView, &dialog),
         proxyViewport = new Viewport(&proxyView, &dialog);
    for (auto view : {originalViewport, proxyViewport}) {
        view->readOnly = true;
        view->setMinimumSize(230, 210);
        views->addWidget(view);
        view->fit();
    }
    auto slider = new QSlider(Qt::Horizontal, &dialog);
    slider->setObjectName("physicsProxyFrameSlider");
    slider->setRange(source.first, source.last);
    slider->setValue(sourceFrame);
    bindUiText(slider,"toolTip","Source frame; both views use the same time");
    layout->addWidget(slider);
    connect(slider, &QSlider::valueChanged, &dialog, [&](int frame) {
        sourceView.seek(frame);
        if (!proxyView.scene.nodes.empty())
            proxyView.seek(qRound(double(frame) * proxyView.scene.fps / source.fps));
    });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    auto open = buttons->button(QDialogButtonBox::Open);
    bindUiText(open,"text","Open proxy as scene");
    open->setObjectName("openPhysicsProxyButton");
    open->setEnabled(false);
    auto preview = [&] {
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool ready = preparePhysicsProxy(source, proxy, measurement, error, fps->currentData().toInt());
        QApplication::restoreOverrideCursor();
        open->setEnabled(ready);
        if (!ready) {
            info->setText(error);
            return;
        }
        proxyView.replace(proxy);
        proxyView.select(sourceSelection);
        proxyView.seek(qRound(double(slider->value()) * proxy.fps / source.fps));
        proxyViewport->fit();
        info->setText(uiText("Approximate active-clip copy ? geometry error %1 mm ? joint error %2 "
                              "mm\nLeft: source. Right: rigid proxy. Source is preserved.")
                          .arg(measurement["maximumWorldGeometryErrorM"].toDouble() * 1000, 0, 'g', 5)
                          .arg(measurement["maximumWorldJointErrorM"].toDouble() * 1000, 0, 'g', 5));
    };
    connect(calculate, &QPushButton::clicked, &dialog, preview);
    connect(fps, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&] {
        open->setEnabled(false);
        QSettings().setValue("physics/proxyFps", fps->currentData());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto current = workspace.find(sourceId);
        if (!current || current->motionRevision != sourceRevision) {
            bindUiText(info,"text","Source changed. Reopen preparation to measure the current scene.");
            open->setEnabled(false);
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    preview();
    if (dialog.exec() == QDialog::Accepted) {
        appendScene(std::move(proxy), {}, false);
        document().edit("Prepared physical proxy", [](Scene &) {}, false);
    }
}
void Window::physicsReportDialog() {
    auto state = physicsTaskState();
    auto r = hasPhysicsResult() ? state["report"].toObject() : document().scene.physicsResult;
    QDialog dialog(this);
    dialog.setObjectName("physicsReportDialog");
    bindUiText(&dialog,"windowTitle","Physics report · Jester");
    dialog.resize(650, 430);
    auto l = new QVBoxLayout(&dialog);
    auto status = new QLabel(
        r.isEmpty() ? uiText("No computed report. Calculate a preview first.")
                    : r["backend"].toString() + " · " + r["status"].toString() +
                          (r["validationStale"].toBool() ? uiText(" · STALE (edited since validation)") : "") + uiText("\n") +
                          r["stopReason"].toString(),
        &dialog);
    status->setWordWrap(true);
    l->addWidget(status);
    auto table = new QTableWidget(0, 4, &dialog);
    table->setObjectName("physicsReportTable");
    bindHeaders(table,{"Metric", "Before", "After", "Tolerance"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto normalization = r["normalization"].toObject();
    double H =
        normalization["heightM"].toDouble(document().scene.height() * document().scene.physics.metersPerUnit);
    auto row = [&](QString name, QString before, QString after, QString tolerance) {
        int i = table->rowCount();
        table->insertRow(i);
        for (int c = 0; c < 4; ++c)
            table->setItem(i, c, new QTableWidgetItem(QStringList{uiText(name),before,uiText(after),uiText(tolerance)}[c]));
        return i;
    };
    auto n = [](double v) { return QString::number(v, 'g', 5); };
    auto value = [&](const QString &key, double scale = 1.) {
        return r[key].isDouble() ? n(r[key].toDouble() * scale) : QString("—");
    };
    auto sampled = r["sampledAnimationInverse"].toObject();
    auto sampledValue = [&](QString side, QString key) {
        QJsonValue v = sampled[side].toObject()[key];
        return v.isDouble() ? n(v.toDouble()) : QString("—");
    };
    row("Sampled inverse root force RMS (N)", sampledValue("before", "rootForceRmsN"),
        sampledValue("after", "rootForceRmsN"), n(normalization["rootForceRmsToleranceN"].toDouble()));
    row("Sampled inverse root moment RMS (Nm)", sampledValue("before", "rootTorqueRmsNm"),
        sampledValue("after", "rootTorqueRmsNm"), "0.03 M g H");
    row("Native inverse root force RMS (N)", "—",
        value(r["reportSchemaVersion"].toInt() >= 2 ? "nativeInverseRootForceRmsN" : "afterRootForceRmsN"),
        n(normalization["rootForceRmsToleranceN"].toDouble()));
    row("Native inverse root moment RMS (Nm)", "—",
        value(r["reportSchemaVersion"].toInt() >= 2 ? "nativeInverseRootTorqueRmsNm"
                                                    : "afterRootTorqueRmsNm"),
        "0.03 M g H");
    if (r["externalAnchors"].toBool()) {
        row("External anchor force (N)", "", value("externalAnchorForceN"), "external support, preview only");
        row("Anchor moment about root (Nm)", "", value("externalAnchorMomentNm"), "root-local frame");
    }
    row("Planted position error (mm)", "—", value("contactErrorM", 1000),
        n((r["staticHintOnly"].toBool() ? .002 : .01) * H * 1000));
    if (r["contactPeakFrame"].isDouble()) {
        int peak = row("Peak slip frame", "", value("contactPeakFrame"), "Double-click to seek");
        table->item(peak, 0)->setData(Qt::UserRole, r["contactPeakFrame"].toDouble());
    }
    row("Penetration (mm)", "—", value("penetrationM", 1000), n(.005 * H * 1000));
    row("Important pose position (mm)", "—", value("targetErrorM", 1000),
        n((r["staticHintOnly"].toBool() ? .005 : .02) * H * 1000));
    row("Important pose angle (deg)", "—", value("targetErrorDeg"), "5");
    row("Directed limit error (deg)", "—", value("directedLimitErrorDeg"), "0.5");
    row("Ballistic COM error (mm)", value("beforeBallisticErrorM", 1000),
        value("interpolatedBallisticErrorM", 1000), n(.0001 * H * 1000));
    row("Angular momentum drift (%)", value("beforeAngularMomentumRelativeDrift", 100),
        value("angularMomentumRelativeDrift", 100), "1");
    row("End COM speed (m/s)", "—", value("endComSpeedMS"), r["endAtRest"].toBool() ? "0.01 sqrt(g H)" : "—");
    row("End segment angular speed (rad/s)", "—", value("endAngularSpeedRadS"),
        r["endAtRest"].toBool() ? "0.1" : "—");
    auto replay = r["independentReplay"].toObject();
    auto replayValue = [&](const QString &key) {
        return replay[key].isDouble() ? n(replay[key].toDouble()) : QString("—");
    };
    row("Impulse balance force RMS (N)", "—", replayValue("linearMomentumBalanceRmsN"),
        n(normalization["rootForceRmsToleranceN"].toDouble()));
    row("Impulse balance moment RMS (Nm)", "—", replayValue("angularMomentumBalanceRmsNm"), "0.03 M g H");
    row("Objective", value("initialObjective"), value("finalObjective"), "goals + dynamics");
    const auto domains = r["validationDomains"].toObject();
    for (auto it = domains.begin(); it != domains.end(); ++it) {
        auto domain = it.value().toObject();
        int index = row(it.key(), "", domain["status"].toString(), domain["methodVersion"].toString());
        table->item(index, 0)->setToolTip(domain["reason"].toString());
    }
    auto representation = r["trajectoryRepresentation"].toObject();
    if (!representation.isEmpty()) {
        row("Trajectory FPS / actual knots", "",
            n(representation["nominalFps"].toDouble()) + " / " +
                n(representation["physicalKnots"].toDouble()),
            "Extra contact-event keys retained");
        row("Trajectory interpolation", "", representation["methodVersion"].toString(),
            "Event samples retained in engine-compatible GLB/FBX export");
    }
    for (auto item : r["intervalBoundaries"].toObject()["boundaries"].toArray()) {
        auto boundary = item.toObject();
        int index = row("Boundary at source frame " + n(boundary["sourceFrame"].toDouble()), "",
                        n(boundary["linearVelocityErrorMS"].toDouble()) + " m/s / " +
                            n(boundary["angularVelocityErrorRadS"].toDouble()) + " rad/s",
                        boundary["compatible"].toBool() ? "compatible" : "velocity/pose conflict");
        table->item(index, 0)->setData(Qt::UserRole, boundary["sourceFrame"].toDouble());
    }
    auto represented = r["measurements"].toObject()["bakedImpulse"].toObject();
    auto representedValue = [&](QString side, QString key) {
        auto v = represented[side].toObject()[key];
        return v.isDouble() ? n(v.toDouble()) : QString("?");
    };
    if (!represented.isEmpty()) {
        row("Baked window force RMS (N)", representedValue("before", "rootForceRmsN"),
            representedValue("after", "rootForceRmsN"),
            n(normalization["rootForceRmsToleranceN"].toDouble()));
        row("Baked window force peak (N)", representedValue("before", "rootForcePeakN"),
            representedValue("after", "rootForcePeakN"), "0.10 M g");
        row("Required window torque / limit", representedValue("before", "requiredWindowTorqueLimitRatio"),
            representedValue("after", "requiredWindowTorqueLimitRatio"), "1.01");
    }
    row("Elapsed time (ms)", "—", value("elapsedMs"), "budget");
    for (auto item : r["importantFramesMeasured"].toArray()) {
        auto f = item.toObject();
        int index = row("Pose at source frame " + n(f["frame"].toDouble()), "",
                        n(f["positionM"].toDouble() * 1000) + " mm / " + n(f["angleDeg"].toDouble()) + " deg",
                        n(.02 * H * 1000) + " mm / 5 deg");
        table->item(index, 0)->setData(Qt::UserRole, f["frame"].toDouble());
        table->item(index, 0)->setToolTip(uiText("Double-click to seek this measured frame."));
    }
    connect(table, &QTableWidget::cellDoubleClicked, &dialog, [&](int index, int) {
        auto frame = table->item(index, 0)->data(Qt::UserRole);
        if (frame.isValid()) {
            document().seek(qRound(frame.toDouble() * document().scene.fps /
                                   r["sourceFps"].toDouble(document().scene.fps)));
            dialog.accept();
        }
    });
    l->addWidget(table);
    auto warnings = new QLabel(r["warnings"].toArray().isEmpty() ? r["dynamicValidation"].toString()
                                                                 : r["warnings"].toArray().first().toString(),
                               &dialog);
    warnings->setWordWrap(true);
    l->addWidget(warnings);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    l->addWidget(buttons);
    dialog.exec();
}
} // namespace trickster
