#include "ui/widgets/localized_ui.h"
#include "ui/widgets/icons.h"
#include "ui/window/window.h"
#include <QApplication>
#include <QDockWidget>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace trickster {
void Window::buildAdvancedPanels() {
    auto shapes = translatedWidget<QDockWidget>("Blend shapes", this);
    shapes->setObjectName("shapesDock");
    shapeTree = new QTreeWidget(shapes);
    shapeTree->setObjectName("shapeTree");
    shapeTree->setColumnCount(2);
    bindHeaders(shapeTree,{"Shape", "Weight"});
    shapeTree->setRootIsDecorated(false);
    shapeTree->setColumnWidth(0, 130);
    shapes->setWidget(shapeTree);
    addDockWidget(Qt::RightDockWidgetArea, shapes);
    auto physicsDock = translatedWidget<QDockWidget>("Physics", this);
    physicsDock->setObjectName("physicsDock");
    auto scroll = new QScrollArea(physicsDock);
    scroll->setObjectName("physicsScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto body = new QWidget;
    auto l = new QVBoxLayout(body);
    l->setContentsMargins(10, 8, 10, 8);
    l->setSpacing(8);
    auto world = translatedWidget<QGroupBox>("World", body);
    auto form = new QFormLayout(world);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    auto field = [&](QString name, float value, float min, float max, QString suffix = {}) {
        auto box = new QDoubleSpinBox(body);
        box->setObjectName(name);
        box->setRange(min, max);
        box->setDecimals(3);
        box->setValue(value);
        box->setKeyboardTracking(false);
        box->setMinimumWidth(70);
        box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        box->setSuffix(suffix);
        return box;
    };
    gravityEdit = field("physicsGravity", 9.81f, 0, 1000, " m/s²");
    unitEdit = field("physicsUnits", 1, .00001f, 10000, " m/u");
    unitEdit->setDecimals(5);
    groundEdit = field("physicsGround", 0, -100000, 100000, " u");
    frictionEdit = field("physicsFriction", .8f, 0, 10);
    addTranslatedRow(form,"Gravity", gravityEdit);
    addTranslatedRow(form,"Scale", unitEdit);
    addTranslatedRow(form,"Ground Y", groundEdit);
    addTranslatedRow(form,"Friction", frictionEdit);
    auto selfCollision = translatedWidget<QCheckBox>("Self collisions", world);
    selfCollision->setObjectName("selfCollisionCheck");
    form->addRow(selfCollision);
    connect(selfCollision, &QCheckBox::toggled, this, [this](bool on) {
        if (!refreshing)
            document().edit("Self collisions", [on](Scene &s) { s.physics.selfCollision = on; }, false);
    });
    l->addWidget(world);
    auto settingsChanged = [this] {
        if (refreshing)
            return;
        const auto &p = document().scene.physics;
        if (std::abs(p.gravity - gravityEdit->value()) < .00051 &&
            std::abs(p.metersPerUnit - unitEdit->value()) < .0000051 &&
            std::abs(p.ground - groundEdit->value()) < .00051 &&
            std::abs(p.friction - frictionEdit->value()) < .00051)
            return;
        document().edit(
            "Physics settings",
            [&](Scene &s) {
                s.physics.gravity = float(gravityEdit->value());
                s.physics.metersPerUnit = float(unitEdit->value());
                s.physics.ground = float(groundEdit->value());
                s.physics.friction = float(frictionEdit->value());
            },
            false);
    };
    for (auto box : {gravityEdit, unitEdit, groundEdit, frictionEdit})
        connect(box, &QDoubleSpinBox::editingFinished, this, settingsChanged);
    auto segment = translatedWidget<QGroupBox>("Selected body", body);
    auto sf = new QFormLayout(segment);
    massEdit = field("bodyMass", 1, .000001f, 100000, " kg");
    massEdit->setDecimals(5);
    radiusEdit = field("bodyRadius", .04f, .000001f, 10000, " u");
    radiusEdit->setDecimals(5);
    addTranslatedRow(sf,"Mass", massEdit);
    addTranslatedRow(sf,"Radius", radiusEdit);
    auto vector = [&](std::array<QDoubleSpinBox *, 3> &boxes, QString prefix) {
        auto row = new QWidget;
        auto layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        for (int i = 0; i < 3; ++i) {
            boxes[i] = field(prefix + QString("XYZ")[i], 0, -100000, 100000);
            boxes[i]->setPrefix(QString("XYZ")[i] + uiText(" "));
            boxes[i]->setMinimumWidth(60);
            layout->addWidget(boxes[i]);
        }
        return row;
    };
    addTranslatedRow(sf,"COM", vector(bodyCenter, "bodyCenter"));
    addTranslatedRow(sf,"Tip", vector(bodyEnd, "bodyEnd"));
    sf->setRowWrapPolicy(QFormLayout::WrapAllRows);
    l->addWidget(segment);
    auto bodyChanged = [this] {
        if (refreshing || document().selected < 0)
            return;
        auto rig =
            document().scene.bodies.empty() ? fittedPhysicsRig(document().scene) : document().scene.bodies;
        auto found = std::find_if(rig.begin(), rig.end(),
                                  [this](const RigidSegment &b) { return b.node == document().selected; });
        if (found == rig.end())
            return;
        bool different = std::abs(found->mass - massEdit->value()) > .0000051 ||
                         std::abs(found->radius - radiusEdit->value()) > .0000051;
        for (int i = 0; i < 3; ++i)
            different = different || std::abs(found->center[i] - bodyCenter[i]->value()) > .00051 ||
                        std::abs(found->end[i] - bodyEnd[i]->value()) > .00051;
        if (!different)
            return;
        document().edit(
            "Rigid body parameters",
            [&](Scene &s) {
                if (s.bodies.empty())
                    s.bodies = fittedPhysicsRig(s);
                for (auto &b : s.bodies)
                    if (b.node == document().selected) {
                        b.mass = float(massEdit->value());
                        b.radius = float(radiusEdit->value());
                        for (int i = 0; i < 3; ++i) {
                            b.center[i] = float(bodyCenter[i]->value());
                            b.end[i] = float(bodyEnd[i]->value());
                        }
                    }
            },
            false);
    };
    connect(massEdit, &QDoubleSpinBox::editingFinished, this, bodyChanged);
    connect(radiusEdit, &QDoubleSpinBox::editingFinished, this, bodyChanged);
    for (int i = 0; i < 3; ++i) {
        connect(bodyCenter[i], &QDoubleSpinBox::editingFinished, this, bodyChanged);
        connect(bodyEnd[i], &QDoubleSpinBox::editingFinished, this, bodyChanged);
    }
    auto fit = translatedIconWidget<QPushButton>(icon(Glyph::Settings),"Fit capsules · 70 kg", body);
    fit->setObjectName("fitPhysicsButton");
    bindUiText(fit,"toolTip","Create a starting capsule rig from the skeleton. Adjust masses, centers and sizes for "
                    "your character.");
    auto massProfile = new QComboBox(body);
    massProfile->setObjectName("physicsMassProfile");
    addTranslatedItems(massProfile,{"Geometry estimate", "Human mass estimate"});
    addTranslatedRow(form,"Mass preset", massProfile);
    bindUiText(massProfile,"toolTip","Both presets are estimates. The human preset uses major-role mass fractions; "
                            "review actual segment dimensions/inertias.");
    connect(fit, &QPushButton::clicked, this, [this, massProfile] {
        applyOperation("physics_body",
                       {{"operation", "fit"},
                        {"totalMass", 70},
                        {"massProfile", massProfile->currentIndex() == 1 ? "human" : "geometry"}},
                       "Fit estimated physical model");
    });
    auto interval = translatedWidget<QGroupBox>("Interval", body);
    auto range = new QHBoxLayout(interval);
    range->setContentsMargins(5, 3, 5, 3);
    physicsFrom = new QSpinBox(body);
    physicsFrom->setObjectName("physicsFrom");
    physicsTo = new QSpinBox(body);
    physicsTo->setObjectName("physicsTo");
    range->addWidget(translatedWidget<QLabel>("In"));
    range->addWidget(physicsFrom);
    range->addWidget(translatedWidget<QLabel>("Out"));
    range->addWidget(physicsTo);
    l->addWidget(interval);
    auto follow = translatedWidget<QCheckBox>("Follow animation", body);
    follow->setObjectName("physicsFollowAnimation");
    bindUiText(follow,"toolTip","Apply limited internal joint torques on every substep. No root/world stabilizer; "
                       "balance is not guaranteed.");
    auto kp = field("physicsStiffness", 80, 0, 10000, " Nm/rad"),
         kd = field("physicsDamping", 8, 0, 1000, " Nms/rad"),
         torque = field("physicsMaxTorque", 100, 0, 10000, " Nm");
    addTranslatedRow(form,"Stiffness", kp);
    addTranslatedRow(form,"Damping", kd);
    addTranslatedRow(form,"Torque limit", torque);
    auto substeps = new QSpinBox(body), solverIterations = new QSpinBox(body);
    substeps->setObjectName("physicsSubsteps");
    substeps->setRange(1, 512);
    solverIterations->setObjectName("physicsSolverIterations");
    solverIterations->setRange(1, 1000);
    addTranslatedRow(form,"Substeps / frame", substeps);
    addTranslatedRow(form,"Contact iterations", solverIterations);
    for (auto spin : {substeps, solverIterations})
        connect(spin, &QSpinBox::editingFinished, this, [this, substeps, solverIterations] {
            if (!refreshing && (document().scene.physics.substeps != substeps->value() ||
                                document().scene.physics.iterations != solverIterations->value()))
                applyOperation("physics_settings",
                               {{"settings", QJsonObject{{"substeps", substeps->value()},
                                                         {"iterations", solverIterations->value()}}}},
                               "Numerical physics settings");
        });
    auto contactTime = field("physicsContactTime", .01f, .0001f, 1, " s"),
         contactDamping = field("physicsContactDamping", 1, .01f, 10);
    contactTime->setDecimals(5);
    addTranslatedRow(form,"Contact time", contactTime);
    addTranslatedRow(form,"Contact damping", contactDamping);
    auto anchors = translatedWidget<QCheckBox>("External anchors (preview)", body);
    anchors->setObjectName("physicsExternalAnchors");
    bindUiText(anchors,"toolTip","Legacy pin/contact constraints with reported external forces. Dynamics correction "
                        "always uses real geom contacts.");
    form->addRow(anchors);
    for (auto box : {contactTime, contactDamping})
        connect(box, &QDoubleSpinBox::editingFinished, this, [this, contactTime, contactDamping] {
            if (!refreshing)
                applyOperation("physics_settings",
                               {{"settings", QJsonObject{{"contactTime", contactTime->value()},
                                                         {"contactDamping", contactDamping->value()}}}},
                               "Contact model");
        });
    connect(anchors, &QCheckBox::toggled, this, [this](bool on) {
        if (!refreshing)
            applyOperation("physics_settings", {{"settings", QJsonObject{{"externalAnchors", on}}}},
                           "External anchor mode");
    });

    l->addWidget(follow);
    connect(follow, &QCheckBox::toggled, this, [this](bool on) {
        if (!refreshing)
            document().edit("Physics follow mode", [&](Scene &s) { s.physics.followAnimation = on; }, false);
    });
    auto pdChanged = [this, kp, kd, torque] {
        if (refreshing)
            return;
        const auto &current = document().scene.physics;
        if (std::abs(current.stiffness - kp->value()) < .00051 &&
            std::abs(current.damping - kd->value()) < .00051 &&
            std::abs(current.maxTorque - torque->value()) < .00051)
            return;
        document().edit(
            "Physics joint drives",
            [&](Scene &s) {
                s.physics.stiffness = float(kp->value());
                s.physics.damping = float(kd->value());
                s.physics.maxTorque = float(torque->value());
            },
            false);
    };
    for (auto box : {kp, kd, torque})
        connect(box, &QDoubleSpinBox::editingFinished, this, pdChanged);
    auto buttons = new QHBoxLayout;
    auto dynamics = translatedIconWidget<QPushButton>(icon(Glyph::Physics),"Simulate interval", body),
         ballistic = translatedIconWidget<QPushButton>(icon(Glyph::Ballistic),"Correct flight", body);
    dynamics->setObjectName("simulatePhysicsButton");
    ballistic->setObjectName("ballisticPhysicsButton");
    bindUiText(dynamics,"toolTip","MuJoCo native CPU simulation: gravity, capsule/ground collisions, joint "
                         "constraints and planted contacts. Initial velocity comes from the animation.");
    bindUiText(ballistic,"toolTip","Correct COM over a free-flight interval using gravity and endpoint times. Release "
                          "world pins and keep planted contacts outside the interval.");
    buttons->addWidget(dynamics);
    buttons->addWidget(ballistic);
    l->addLayout(buttons);
    connect(dynamics, &QPushButton::clicked, this, [this] { previewPhysics(false); });
    connect(ballistic, &QPushButton::clicked, this, [this] { previewPhysics(true); });
    auto ghost = translatedWidget<QCheckBox>("Show calculated result", body);
    ghost->setObjectName("physicsPreviewCheck");
    ghost->setChecked(true);
    connect(ghost, &QCheckBox::toggled, this, [this](bool on) {
        viewport->showPhysicsPreview = on;
        viewport->update();
    });
    l->addWidget(ghost);
    physicsStats = translatedWidget<QLabel>("No preview", body);
    physicsStats->setObjectName("physicsStats");
    physicsStats->setWordWrap(true);
    physicsStats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(physicsStats);
    physicsProgress = new QProgressBar(body);
    physicsProgress->setObjectName("physicsProgress");
    physicsProgress->setRange(0, 100);
    physicsProgress->hide();
    l->addWidget(physicsProgress);
    cancelPhysics = translatedWidget<QPushButton>("Cancel", body);
    cancelPhysics->setObjectName("cancelPhysicsButton");
    cancelPhysics->hide();
    l->addWidget(cancelPhysics);
    connect(cancelPhysics, &QPushButton::clicked, this, &Window::cancelPhysicsTask);
    connect(&physicsPoll, &QTimer::timeout, this, &Window::pollPhysicsTask);
    connect(&workspace, &Workspace::sceneReplaced, this, [this] {
        physicsPreview.reset();
        viewport->physicsPreview.reset();
        if (applyPhysics)
            applyPhysics->setEnabled(false);
        if (physicsJobs.state()["status"] != "idle")
            pollPhysicsTask();
    });
    connect(&workspace, &Workspace::historyChanged, this, [this] { pollPhysicsTask(); });
    applyPhysics = translatedIconWidget<QPushButton>(icon(Glyph::Bake),"Open result as scene", body);
    applyPhysics->setObjectName("applyPhysicsButton");
    applyPhysics->setEnabled(false);
    connect(applyPhysics, &QPushButton::clicked, this, [this] {
        commitPendingEditor();
        document().play(false);
        if (auto preview = physicsResult()) {
            auto result = *preview;
            appendScene(std::move(result));
            document().edit("Physics result", [](Scene &) {}, false);
            report(QString("Physics result opened. Ctrl+S saves the project; Ctrl+Shift+E exports."));
        }
    });
    l->addWidget(applyPhysics);
    for (QWidget *widget :
         std::initializer_list<QWidget *>{world, segment, fit, interval, ghost, physicsStats, applyPhysics})
        l->removeWidget(widget);
    l->removeItem(buttons);
    buttons->setParent(nullptr);
    auto assistant = new QGridLayout;
    auto whole = translatedIconWidget<QPushButton>(icon(Glyph::Move),"Pose targets", body),
         model = translatedIconWidget<QPushButton>(icon(Glyph::Bones),"Model", body),
         optimize = translatedIconWidget<QPushButton>(icon(Glyph::Physics),"Correct motion", body);
    whole->setObjectName("wholeBodyButton");
    model->setObjectName("physicsModelButton");
    optimize->setObjectName("optimizeDynamicsButton");
    bindUiText(whole,"toolTip","Whole-body targets and COM projection (kinematic)");
    bindUiText(model,"toolTip","Validate mass, inertia, shapes and export MJCF");
    bindToolTip(optimize,"Optimize internal controls on the interval; real contacts, independent forward replay");
    assistant->addWidget(whole, 0, 0);
    assistant->addWidget(model, 0, 1);
    assistant->addWidget(optimize, 1, 0, 1, 2);
    connect(whole, &QPushButton::clicked, this, &Window::showWholeBodyDialog);
    connect(model, &QPushButton::clicked, this, &Window::physicsModelDialog);
    connect(optimize, &QPushButton::clicked, this, [this] { physicsCalculationDialog("optimize"); });
    l->addLayout(assistant);
    auto overlays = translatedWidget<QCheckBox>("Bodies / forces", body);
    overlays->setObjectName("physicsOverlaysCheck");
    connect(overlays, &QCheckBox::toggled, this, [this](bool on) {
        viewport->showPhysicsModel = on;
        viewport->update();
    });
    l->addWidget(overlays);
    l->addWidget(interval);
    l->addLayout(buttons);
    auto previewRow = new QHBoxLayout;
    previewRow->addWidget(ghost);
    auto reportButton = new QToolButton(body);
    reportButton->setObjectName("physicsReportButton");
    reportButton->setIcon(icon(Glyph::Settings));
    bindUiText(reportButton,"toolTip","Before/after metrics and acceptance tolerances");
    connect(reportButton, &QToolButton::clicked, this, &Window::physicsReportDialog);
    previewRow->addWidget(reportButton);
    previewRow->addStretch();
    l->addLayout(previewRow);
    l->addWidget(applyPhysics);
    l->addWidget(physicsStats);
    form->addRow(fit);
    auto details = [&](QString name, QWidget *content) {
        auto toggle = new QToolButton(body);
        toggle->setObjectName(name == "World settings" ? "worldSettingsButton" : "rigidBodyDetailsButton");
        bindUiText(toggle,"text",name);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(Qt::RightArrow);
        toggle->setCheckable(true);
        toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        content->hide();
        connect(toggle, &QToolButton::toggled, content, [toggle, content](bool open) {
            content->setVisible(open);
            toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        });
        l->addWidget(toggle);
        l->addWidget(content);
    };
    details("World settings", world);
    details("Rigid body", segment);
    l->addStretch();
    scroll->setWidget(body);
    physicsDock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, physicsDock);
    auto menu = addTranslatedMenu(menuBar(),"P&hysics");
    auto wholeAction =
        addTranslatedIconAction(menu,icon(Glyph::Move),"Whole-body pose…", this, &Window::showWholeBodyDialog);
    wholeAction->setObjectName("wholeBodyAction");
    wholeAction->setShortcut(QKeySequence("Ctrl+Shift+W"));
    auto modelAction =
        addTranslatedIconAction(menu,icon(Glyph::Bones),"Physical model…", this, &Window::physicsModelDialog);
    modelAction->setObjectName("physicsModelAction");
    auto proxyAction =
        addTranslatedIconAction(menu,icon(Glyph::Bones),"Prepare rigid proxy...", this, &Window::physicsProxyDialog);
    proxyAction->setObjectName("physicsProxyAction");
    auto solveAction = addTranslatedIconAction(menu,icon(Glyph::Physics),"Correct dynamics…", this,
                                       [this] { physicsCalculationDialog("optimize"); });
    solveAction->setObjectName("optimizeDynamicsAction");
    auto conditionsAction = addTranslatedIconAction(menu,icon(Glyph::Ballistic),"Flight conditions…", this,
                                            [this] { physicsCalculationDialog("flight"); });
    conditionsAction->setObjectName("flightConditionsAction");
    auto dynamicsAction = addTranslatedIconAction(menu,icon(Glyph::Physics),"Simulate interval");
    dynamicsAction->setObjectName("simulatePhysicsAction");
    dynamicsAction->setShortcut(QKeySequence("Ctrl+Shift+P"));
    connect(dynamicsAction, &QAction::triggered, this, [this] {
        auto dock = findChild<QDockWidget *>("physicsDock");
        dock->show();
        dock->raise();
        previewPhysics(false);
    });
    auto flightAction = addTranslatedIconAction(menu,icon(Glyph::Ballistic),"Correct flight");
    flightAction->setObjectName("correctFlightAction");
    connect(flightAction, &QAction::triggered, this, [this] {
        auto dock = findChild<QDockWidget *>("physicsDock");
        dock->show();
        dock->raise();
        previewPhysics(true);
    });
    connect(&workspace, &Workspace::sceneReplaced, this, [this] {
        rebuildShapes();
        physicsFrom->setRange(document().scene.first, document().scene.last);
        physicsTo->setRange(document().scene.first, document().scene.last);
        physicsFrom->setValue(document().scene.first);
        physicsTo->setValue(document().scene.last);
    });
    rebuildShapes();
    physicsTo->setValue(document().scene.last);
}
void Window::rebuildShapes() {
    const bool previous = refreshing;
    refreshing = true;
    for (auto editor : morphEditors)
        editor->blockSignals(true);
    for (auto slider : morphSliders)
        slider->blockSignals(true);
    shapeTree->clear();
    morphBindings.clear();
    morphEditors.clear();
    morphSliders.clear();
    for (int node = 0; node < int(document().scene.nodes.size()); ++node) {
        const Mesh *mesh = nullptr;
        for (const auto &m : document().scene.meshes)
            if (m.node == node && !m.targets.empty()) {
                mesh = &m;
                break;
            }
        if (!mesh)
            continue;
        auto group = new QTreeWidgetItem(shapeTree, {document().scene.nodes[node].name});
        group->setIcon(0, icon(Glyph::Mesh));
        group->setFirstColumnSpanned(true);
        group->setExpanded(true);
        for (int shapeIndex = 0; shapeIndex < int(mesh->targets.size()); ++shapeIndex) {
            auto item = new QTreeWidgetItem(group, {mesh->targets[shapeIndex].name});
            auto row = new QWidget;
            auto layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 2, 0, 2);
            layout->setSpacing(3);
            auto slider = new QSlider(Qt::Horizontal, row);
            slider->setRange(-100, 200);
            slider->setMinimumWidth(45);
            slider->setObjectName(QString("morphSlider_%1_%2").arg(node).arg(shapeIndex));
            auto spin = new QDoubleSpinBox(row);
            spin->setRange(-10000, 10000);
            spin->setDecimals(1);
            bindUiText(spin,"suffix"," %");
            spin->setKeyboardTracking(false);
            spin->setObjectName(QString("morphWeight_%1_%2").arg(node).arg(shapeIndex));
            spin->setFixedWidth(80);
            layout->addWidget(slider, 1);
            layout->addWidget(spin);
            shapeTree->setItemWidget(item, 1, row);
            morphBindings.emplace_back(node, shapeIndex);
            morphEditors.push_back(spin);
            morphSliders.push_back(slider);
            connect(slider, &QSlider::sliderPressed, this, [this] { document().begin(); });
            connect(slider, &QSlider::sliderReleased, this, [this] { document().commit("Blend shape"); });
            connect(slider, &QSlider::valueChanged, this, [this, node, shapeIndex, slider](int value) {
                if (refreshing)
                    return;
                auto edit = [&](Scene &s) {
                    auto w = s.morphWeights(node);
                    w[shapeIndex] = float(value) / 100;
                    s.morphs[node] = w;
                };
                if (slider->isSliderDown()) {
                    edit(document().scene);
                    emit document().changed();
                } else
                    document().edit("Blend shape", edit);
            });
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this, node, shapeIndex, spin] {
                if (refreshing || node >= int(document().scene.nodes.size()) ||
                    shapeIndex >= int(document().scene.morphWeights(node).size()))
                    return;
                float value = float(spin->value() / 100);
                if (std::abs(document().scene.morphWeights(node)[shapeIndex] - value) < 1e-6f)
                    return;
                document().edit("Blend shape", [&](Scene &s) {
                    auto w = s.morphWeights(node);
                    w[shapeIndex] = value;
                    s.morphs[node] = w;
                });
            });
        }
    }
    if (morphBindings.empty()) {
        auto item = new QTreeWidgetItem(shapeTree, {uiText("No blend shapes")});
        item->setFlags(Qt::NoItemFlags);
    }
    refreshing = previous;
}
void Window::refreshPanels() {
    if (!gravityEdit)
        return;
    auto &s = document().scene;
    findChild<QSpinBox *>("physicsSubsteps")->setValue(s.physics.substeps);
    findChild<QSpinBox *>("physicsSolverIterations")->setValue(s.physics.iterations);
    findChild<QDoubleSpinBox *>("physicsContactTime")->setValue(s.physics.contactTime);
    findChild<QDoubleSpinBox *>("physicsContactDamping")->setValue(s.physics.contactDamping);
    findChild<QCheckBox *>("physicsExternalAnchors")->setChecked(s.physics.externalAnchors);
    findChild<QCheckBox *>("physicsFollowAnimation")->setChecked(s.physics.followAnimation);
    findChild<QDoubleSpinBox *>("physicsStiffness")->setValue(s.physics.stiffness);
    findChild<QDoubleSpinBox *>("physicsDamping")->setValue(s.physics.damping);
    findChild<QDoubleSpinBox *>("physicsMaxTorque")->setValue(s.physics.maxTorque);
    findChild<QCheckBox *>("selfCollisionCheck")->setChecked(s.physics.selfCollision);
    gravityEdit->setValue(s.physics.gravity);
    unitEdit->setValue(s.physics.metersPerUnit);
    groundEdit->setValue(s.physics.ground);
    frictionEdit->setValue(s.physics.friction);
    physicsFrom->setRange(s.first, s.last);
    physicsTo->setRange(s.first, s.last);
    for (size_t i = 0; i < morphBindings.size(); ++i) {
        auto [node, shapeIndex] = morphBindings[i];
        if (node >= int(s.nodes.size()))
            continue;
        auto w = s.morphWeights(node);
        if (shapeIndex >= int(w.size()))
            continue;
        QSignalBlocker block(morphSliders[i]);
        morphEditors[i]->setValue(w[shapeIndex] * 100);
        morphSliders[i]->setValue(int(std::round(w[shapeIndex] * 100)));
    }
    if(s.bodies.empty() && (fittedBodiesDocument!=document().id || fittedBodiesRevision!=document().profileRevision+document().meshRevision+document().structureRevision)) {
        fittedBodiesCache=fittedPhysicsRig(s);fittedBodiesDocument=document().id;fittedBodiesRevision=document().profileRevision+document().meshRevision+document().structureRevision;
    }
    const auto &bodies=s.bodies.empty()?fittedBodiesCache:s.bodies;
    auto found = std::find_if(bodies.begin(), bodies.end(),
                              [&](const RigidSegment &b) { return b.node == document().selected; });
    bool valid = found != bodies.end();
    massEdit->parentWidget()->setEnabled(valid);
    if (!s.physicsResult.isEmpty() && !hasPhysicsResult()) {
        auto r = s.physicsResult;
        physicsStats->setText(r["status"].toString() + (r["validationStale"].toBool() ? uiText(" · stale") : "") +
                              uiText("\n") + r["stopReason"].toString());
    }
    if (valid) {
        massEdit->setValue(found->mass);
        radiusEdit->setValue(found->radius);
        for (int i = 0; i < 3; ++i) {
            bodyCenter[i]->setValue(found->center[i]);
            bodyEnd[i]->setValue(found->end[i]);
        }
    }
    for (auto name :
         {"wholeBodyAction", "physicsModelAction", "optimizeDynamicsAction", "flightConditionsAction"})
        findChild<QAction *>(name)->setEnabled(!s.nodes.empty());
    for (auto name : {"wholeBodyButton", "physicsModelButton", "optimizeDynamicsButton"})
        findChild<QPushButton *>(name)->setEnabled(!s.nodes.empty());
    findChild<QAction *>("simulatePhysicsAction")->setEnabled(!s.nodes.empty());
    findChild<QAction *>("correctFlightAction")->setEnabled(!s.nodes.empty());
    for (auto name : {"simulatePhysicsButton", "ballisticPhysicsButton", "fitPhysicsButton"})
        findChild<QPushButton *>(name)->setEnabled(!s.nodes.empty());
}
void Window::previewPhysics(bool ballistic) {
    commitPendingEditor();
    document().play(false);
    QString error;
    if (!startPhysicsTask(ballistic, physicsFrom->value(), physicsTo->value(), error))
        report(error);
}
} // namespace trickster
