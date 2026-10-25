#include "icons.h"
#include "window.h"
#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
namespace trickster {
void Window::buildMotionPanel() {
    auto dock = new QDockWidget("Motion", this);
    dock->setObjectName("motionDock");
    auto body = new QWidget(dock);
    auto layout = new QVBoxLayout(body);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    auto profileButton = new QPushButton(icon(Glyph::Settings), "Profile", body);
    profileButton->setObjectName("characterProfileButton");
    profileButton->setToolTip(
        "Assign humanoid roles and explicit IK chains, including helper/twist ancestors.");
    auto setupRow = new QHBoxLayout;
    setupRow->addWidget(profileButton);
    connect(profileButton, &QPushButton::clicked, this, &Window::characterProfileDialog);
    auto limitButton = new QPushButton(icon(Glyph::Rotate), "Joint limit", body);
    limitButton->setObjectName("jointLimitButton");
    setupRow->addWidget(limitButton);
    auto contacts = new QToolButton(body);
    contacts->setIcon(icon(Glyph::Feet));
    contacts->setObjectName("motionContactsButton");
    contacts->setToolTip("Edit contacts and ground support");
    setupRow->addWidget(contacts);
    connect(contacts, &QToolButton::clicked, this, &Window::contactDialog);
    auto physics = new QToolButton(body);
    physics->setIcon(icon(Glyph::Physics));
    physics->setObjectName("motionPhysicsButton");
    physics->setToolTip("Passive / Follow preview and independent results");
    setupRow->addWidget(physics);
    connect(physics, &QToolButton::clicked, this, [this] {
        auto dock = findChild<QDockWidget *>("physicsDock");
        dock->show();
        dock->raise();
    });
    layout->addLayout(setupRow);
    connect(limitButton, &QPushButton::clicked, this, &Window::jointLimitDialog);
    auto sourceForm = new QFormLayout;
    motionClip = new QComboBox(body);
    motionClip->setObjectName("motionClip");
    motionClip->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    motionClip->setMinimumContentsLength(12);
    motionClip->setToolTip("Open another imported clip in a separate scene, preserving this scene's edits.");
    sourceForm->addRow("Clip", motionClip);
    motionBakeFps = new QSpinBox(body);
    motionBakeFps->setObjectName("motionBakeFps");
    motionBakeFps->setRange(1, 240);
    motionBakeFps->setValue(30);
    motionBakeFps->setToolTip(
        "Explicit sampling rate for Bake as new scene; glTF source times remain in seconds.");
    motionBakeFps->setMaximumWidth(70);
    layout->addLayout(sourceForm);
    connect(motionClip, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (refreshing || index < 0)
            return;
        commitPendingEditor();
        if (!document().scene.clips)
            return;
        auto copy = document().scene;
        copy.clearCorrections();
        QString error;
        if (!copy.selectClip(index, error)) {
            report(error, true);
            return;
        }
        appendScene(std::move(copy), {}, false);
    });
    auto layerRow = new QHBoxLayout;
    auto layers = new QComboBox(body);
    layers->setObjectName("motionLayers");
    layers->setMinimumContentsLength(12);
    layers->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layers->setToolTip("Selected recording layer. Layers are evaluated from top to bottom in this list.");
    layerRow->addWidget(layers, 1);
    auto duplicate = new QToolButton(body);
    duplicate->setObjectName("duplicateLayerButton");
    duplicate->setText("⧉");
    duplicate->setToolTip("Duplicate selected layer");
    layerRow->addWidget(duplicate);
    auto up = new QToolButton(body);
    up->setObjectName("layerUpButton");
    up->setText("↑");
    up->setToolTip("Evaluate selected layer earlier");
    layerRow->addWidget(up);
    auto down = new QToolButton(body);
    down->setObjectName("layerDownButton");
    down->setText("↓");
    down->setToolTip("Evaluate selected layer later");
    layerRow->addWidget(down);
    layout->addLayout(layerRow);
    layers->setEditable(true);
    layers->setInsertPolicy(QComboBox::NoInsert);
    auto layerName = layers->lineEdit();
    layerName->setObjectName("motionLayerName");
    layerName->setPlaceholderText("Layer name");
    layerName->setMaxLength(128);
    auto original = new QCheckBox("Original", body);
    original->setObjectName("compareOriginalCheck");
    original->setToolTip("Show the uncorrected source before added layers, contacts and limits. Export/bake "
                         "still use the enabled result.");
    auto keysRow = new QHBoxLayout;
    auto key = new QPushButton(icon(Glyph::Key), "Key", body);
    key->setObjectName("correctionKeyButton");
    key->setToolTip("Record current selected-layer offsets (I)");
    keysRow->addWidget(key);
    auto neutral = new QPushButton("Neutral key", body);
    neutral->setObjectName("neutralCorrectionKeyButton");
    neutral->setToolTip(
        "Return existing correction channels and selected FK/IK chain to zero offsets at the playhead");
    keysRow->addWidget(neutral);
    layout->addLayout(keysRow);
    connect(layers, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!refreshing && index >= 0) {
            commitPendingEditor();
            applyOperation("motion_correction", {{"operation", "select"}, {"layer", index}});
        }
    });
    connect(layerName, &QLineEdit::editingFinished, this, [this, layerName] {
        if (!refreshing && document().scene.correction().present && !layerName->text().trimmed().isEmpty() &&
            layerName->text() != document().scene.correction().name)
            applyOperation("motion_correction", {{"operation", "set"}, {"name", layerName->text().trimmed()}},
                           "Rename layer");
    });
    connect(duplicate, &QToolButton::clicked, this,
            [this] { applyOperation("motion_correction", {{"operation", "duplicate"}}, "Duplicate layer"); });
    connect(up, &QToolButton::clicked, this, [this] {
        applyOperation("motion_correction",
                       {{"operation", "reorder"}, {"index", document().scene.activeLayer - 1}},
                       "Move layer earlier");
    });
    connect(down, &QToolButton::clicked, this, [this] {
        applyOperation("motion_correction",
                       {{"operation", "reorder"}, {"index", document().scene.activeLayer + 1}},
                       "Move layer later");
    });
    connect(original, &QCheckBox::toggled, this, [this](bool on) {
        if (!refreshing)
            applyOperation("motion_correction", {{"operation", "compare"}, {"enabled", on}},
                           "Compare original");
    });
    connect(key, &QPushButton::clicked, this, [this] {
        commitPendingEditor();
        timeline->insertKey();
    });
    connect(neutral, &QPushButton::clicked, this, [this] {
        commitPendingEditor();
        timeline->insertKey(true);
    });
    auto envelope = new QWidget(body);
    envelope->setObjectName("correctionEnvelope");
    auto envelopeToggle = new QToolButton(body);
    envelopeToggle->setObjectName("correctionEnvelopeToggle");
    envelopeToggle->setText("Envelope");
    envelopeToggle->setIcon(icon(Glyph::Expand));
    envelopeToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    envelopeToggle->setCheckable(true);
    envelopeToggle->setToolTip("Edit interval, fades and strength");
    layout->addWidget(envelopeToggle);
    connect(envelopeToggle, &QToolButton::toggled, this, [envelope, envelopeToggle](bool on) {
        envelope->setVisible(on);
        envelopeToggle->setIcon(icon(on ? Glyph::Collapse : Glyph::Expand));
    });
    auto form = new QFormLayout(envelope);
    form->setContentsMargins(0, 0, 0, 0);
    auto row = new QWidget(body);
    auto range = new QHBoxLayout(row);
    range->setContentsMargins(0, 0, 0, 0);
    correctionFrom = new QSpinBox(row);
    correctionFrom->setObjectName("correctionFrom");
    correctionTo = new QSpinBox(row);
    correctionTo->setObjectName("correctionTo");
    range->addWidget(correctionFrom);
    range->addWidget(correctionTo);
    form->addRow("Interval", row);
    auto fades = new QWidget(body);
    auto fl = new QHBoxLayout(fades);
    fl->setContentsMargins(0, 0, 0, 0);
    correctionFadeIn = new QSpinBox(fades);
    correctionFadeIn->setObjectName("correctionFadeIn");
    correctionFadeOut = new QSpinBox(fades);
    correctionFadeOut->setObjectName("correctionFadeOut");
    for (auto f : {correctionFadeIn, correctionFadeOut}) {
        f->setRange(0, 100000);
        f->setValue(5);
        fl->addWidget(f);
    }
    form->addRow("Fade in / out", fades);
    auto create = new QPushButton(icon(Glyph::Key), "New layer", body);
    create->setObjectName("createCorrectionButton");
    create->setToolTip("Create an additive local-space correction. Pose with FK/IK inside the interval; "
                       "source keys stay intact.");
    layout->addWidget(envelope);
    envelope->hide();
    keysRow->insertWidget(0, create);
    correctionEnabled = new QCheckBox("Layer", body);
    correctionEnabled->setObjectName("correctionEnabled");
    correctionEnabled->setToolTip(
        "Enable or bypass this layer. Original compares the whole result with the source.");
    correctionEditing = new QCheckBox("Record", body);
    correctionEditing->setObjectName("correctionEditing");
    correctionEditing->setToolTip(
        "Pose edits affect this layer; turning this off locks posing. Bake a copy to edit "
        "source pose keys at the timeline FPS.");
    correctionStrength = new QDoubleSpinBox(body);
    correctionStrength->setObjectName("correctionStrength");
    correctionStrength->setRange(0, 1);
    correctionStrength->setSingleStep(.1);
    correctionStrength->setDecimals(2);
    form->addRow("Strength", correctionStrength);
    auto controls = new QHBoxLayout;
    controls->addWidget(correctionEnabled);
    controls->addWidget(correctionEditing);
    controls->addWidget(original);
    auto remove = new QToolButton(body);
    remove->setObjectName("removeCorrectionButton");
    remove->setIcon(icon(Glyph::Undo));
    remove->setToolTip("Remove selected layer (undoable); other layers remain active.");
    controls->addWidget(remove);
    connect(remove, &QToolButton::clicked, this, [this] {
        commitPendingEditor();
        applyOperation("motion_correction", {{"operation", "remove"}}, "Remove correction");
    });
    layout->insertLayout(3, controls);
    correctionStatus = new QLabel(body);
    correctionStatus->setObjectName("correctionStatus");
    correctionStatus->setWordWrap(true);
    layout->addWidget(correctionStatus);
    auto bake = new QPushButton(icon(Glyph::Bake), "Bake copy", body);
    bake->setObjectName("bakeCorrectionButton");
    bake->setToolTip(
        "Bake the final motion, including constraints. Keep this source scene and correction editable.");
    auto footer = new QWidget(dock);
    auto bakeRow = new QHBoxLayout(footer);
    bakeRow->setContentsMargins(6, 4, 6, 4);
    bakeRow->addWidget(bake, 1);
    bakeRow->addWidget(new QLabel("fps", body));
    bakeRow->addWidget(motionBakeFps);

    layout->addStretch(1);
    auto scroll = new QScrollArea(dock);
    scroll->setObjectName("motionScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(body);
    auto container = new QWidget(dock);
    auto content = new QVBoxLayout(container);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    content->addWidget(scroll, 1);
    content->addWidget(footer);
    dock->setWidget(container);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    tabifyDockWidget(findChild<QDockWidget *>("skeletonDock"), dock);
    connect(this, &QMainWindow::tabifiedDockWidgetActivated, this, [this, dock](QDockWidget *active) {
        if (active == dock && !compactLayout)
            resizeDocks({dock, findChild<QDockWidget *>("inspectorDock")}, {320, 160}, Qt::Vertical);
    });
    connect(create, &QPushButton::clicked, this, [this] {
        commitPendingEditor();
        if (!applyOperation("motion_correction",
                            {{"operation", "create"},
                             {"first", correctionFrom->value()},
                             {"last", correctionTo->value()},
                             {"fadeIn", correctionFadeIn->value()},
                             {"fadeOut", correctionFadeOut->value()}},
                            "Create correction"))
            return;
        const auto &c = document().scene.correction();
        document().seek((c.first + c.fadeIn + c.last - c.fadeOut) / 2);
        report("Correction active. Pose with W / E; toggle Correction to compare with the source.");
    });
    auto updateParameters = [this] {
        if (refreshing || !document().scene.correction().present)
            return;
        auto c = document().scene.correction();
        c.first = correctionFrom->value();
        c.last = correctionTo->value();
        c.fadeIn = correctionFadeIn->value();
        c.fadeOut = correctionFadeOut->value();
        c.strength = float(correctionStrength->value());
        const auto &previous = document().scene.correction();
        if (c.first == previous.first && c.last == previous.last && c.fadeIn == previous.fadeIn &&
            c.fadeOut == previous.fadeOut && c.strength == previous.strength)
            return;
        Scene check = document().scene;
        check.correction() = c;
        QString error;
        if (!check.validateCorrection(&error)) {
            report(error, true);
            refreshMotionPanel();
            return;
        }
        document().edit(
            "Correction envelope",
            [&](Scene &s) {
                s.correction() = c;
                s.evaluate(float(document().frame));
            },
            false);
    };
    for (auto f : {correctionFrom, correctionTo, correctionFadeIn, correctionFadeOut}) {
        f->setKeyboardTracking(false);
        connect(f, &QSpinBox::editingFinished, this, updateParameters);
    }
    correctionStrength->setKeyboardTracking(false);
    connect(correctionStrength, &QDoubleSpinBox::editingFinished, this, updateParameters);
    connect(correctionEnabled, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing)
            return;
        document().edit(
            "Toggle correction",
            [&](Scene &s) {
                s.correction().enabled = on;
                s.evaluate(float(document().frame));
            },
            false);
    });
    connect(correctionEditing, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing)
            return;
        document().edit("Correction editing", [&](Scene &s) { s.correction().editing = on; }, false);
    });
    connect(bake, &QPushButton::clicked, this, [this] {
        commitPendingEditor();
        if (document().scene.nodes.empty())
            return;
        const auto &source = document().scene;
        int rate = motionBakeFps->value();
        if (qint64(source.last - source.first + 1) * rate / source.fps * source.nodes.size() > 5000000 ||
            float(source.last) * rate / source.fps > 100000) {
            report("Shorten the range or lower Bake FPS.");
            return;
        }
        auto result = source.resampledCopy(rate);
        appendScene(std::move(result));
        document().edit("Baked motion", [](Scene &) {}, false);
    });
    connect(&workspace, &Workspace::changed, this, &Window::refreshMotionPanel);
    connect(&workspace, &Workspace::sceneReplaced, this, [this] {
        QSignalBlocker a(correctionFrom), b(correctionTo);
        correctionFrom->setRange(document().scene.first, document().scene.last);
        correctionTo->setRange(document().scene.first, document().scene.last);
        correctionFrom->setValue(document().scene.first);
        correctionTo->setValue(document().scene.last);
        QSignalBlocker clipsBlock(motionClip);
        motionClip->clear();
        if (document().scene.clips)
            for (const auto &clip : *document().scene.clips)
                motionClip->addItem(clip.name);
        motionClip->setCurrentIndex(document().scene.sourceClip);
        motionBakeFps->setValue(document().scene.fps);
        refreshMotionPanel();
        if (document().scene.clips && document().scene.clips->size() > 1) {
            auto d = findChild<QDockWidget *>("motionDock");
            d->show();
            d->raise();
        }
    });
    auto toggle = dock->toggleViewAction();
    toggle->setObjectName("motionDockToggleAction");
    toggle->setShortcut(QKeySequence("Ctrl+Alt+M"));
    for (auto menu : menuBar()->findChildren<QMenu *>())
        if (menu->title().remove('&') == "View") {
            menu->addAction(toggle);
            break;
        }
    refreshMotionPanel();
}
void Window::refreshMotionPanel() {
    if (!correctionFrom)
        return;
    bool old = refreshing;
    refreshing = true;
    auto &s = document().scene;
    const auto &c = s.correction();
    const bool staticChanged = motionPanelDocument != document().id ||
                               motionPanelRevision != document().motionRevision ||
                               motionPanelOriginal != s.compareOriginal;
    if (staticChanged) {
        motionPanelDocument = document().id;
        motionPanelRevision = document().motionRevision;
        motionPanelOriginal = s.compareOriginal;
        auto layers = findChild<QComboBox *>("motionLayers");
        {
            QSignalBlocker block(layers);
            auto line = layers->lineEdit();
            QString pending = line->text();
            bool typing = line->hasFocus();
            QStringList signature;
            for (const auto &layer : s.layers)
                signature << layer.id + layer.name + QString::number(layer.enabled);
            if (layers->property("layerListSignature").toStringList() != signature) {
                layers->clear();
                for (const auto &layer : s.layers)
                    layers->addItem(icon(layer.enabled ? Glyph::Key : Glyph::Ghost), layer.name);
                layers->setProperty("layerListSignature", signature);
            }
            layers->setCurrentIndex(s.activeLayer);
            if (typing)
                line->setText(pending);
        }
        auto name = findChild<QLineEdit *>("motionLayerName");
        if (!name->hasFocus())
            name->setText(c.present ? c.name : QString());
        name->setEnabled(c.present);
        {
            QSignalBlocker b(findChild<QCheckBox *>("compareOriginalCheck"));
            findChild<QCheckBox *>("compareOriginalCheck")->setChecked(s.compareOriginal);
        }
        findChild<QToolButton *>("duplicateLayerButton")->setEnabled(c.present && s.layers.size() < 32);
        findChild<QToolButton *>("layerUpButton")->setEnabled(s.activeLayer > 0);
        findChild<QToolButton *>("layerDownButton")
            ->setEnabled(s.activeLayer >= 0 && s.activeLayer + 1 < int(s.layers.size()));
        findChild<QPushButton *>("correctionKeyButton")
            ->setEnabled(c.present && c.enabled && c.editing && !s.compareOriginal);
        findChild<QPushButton *>("neutralCorrectionKeyButton")
            ->setEnabled(c.present && c.enabled && c.editing && !s.compareOriginal);
        motionClip->setEnabled(s.clips && !s.clips->empty());
        for (auto box : {correctionFrom, correctionTo}) {
            QSignalBlocker b(box);
            box->setRange(s.first, s.last);
        }
        if (c.present) {
            correctionFrom->setValue(c.first);
            correctionTo->setValue(c.last);
            correctionFadeIn->setValue(c.fadeIn);
            correctionFadeOut->setValue(c.fadeOut);
        }
        findChild<QToolButton *>("removeCorrectionButton")->setEnabled(c.present);
        correctionEnabled->setEnabled(c.present);
        correctionEditing->setEnabled(c.present);
        correctionStrength->setEnabled(c.present);
        correctionEnabled->setChecked(c.present && c.enabled);
        correctionEditing->setChecked(c.present && c.editing);
        correctionStrength->setValue(c.strength);
        auto create = findChild<QPushButton *>("createCorrectionButton");
        create->setEnabled(!s.nodes.empty() && s.layers.size() < 32);
        findChild<QPushButton *>("bakeCorrectionButton")->setEnabled(!s.nodes.empty());
    }
    correctionStatus->setText(s.compareOriginal ? "Original comparison · recording paused"
                              : !c.present      ? "Select an interval, then pose with FK / IK."
                              : !c.enabled
                                  ? "Selected layer disabled"
                                  : QString("%1 · %2 bones · %3%")
                                        .arg(c.world ? "World offsets" : "Local offsets")
                                        .arg(s.sampleLayer(c, float(document().frame)).offsets.size())
                                        .arg(qRound(s.correctionWeight(float(document().frame)) * 100)));
    correctionStatus->setToolTip(!c.enabled  ? "Source motion"
                                 : c.editing ? "Editing correction; source keys preserved"
                                             : "Pose editing locked");
    refreshing = old;
}
} // namespace trickster
