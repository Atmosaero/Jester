#include "io/project/project_format.h"
#include "io/project/project_io.h"
#include "ui/window/window.h"
#include "ui/notes/notes_ui.h"
#include "ui/window/scene_settings_panel.h"
#include "ui/dialogs/preferences_dialog.h"
#include "ui/widgets/localized_ui.h"
#include "settings/action_registry.h"
#include "settings/preferences_service.h"
#include "settings/editor_appearance.h"
#include <QToolButton>
#include <QMenu>
#include "core/scene/edit_service.h"
#include "ui/widgets/icons.h"
#include "trickster_version.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <cmath>
#include <utility>
namespace trickster {
namespace {
QString recordingContextLabel(const Document &document) {
    const auto &scene=document.scene;const auto &layer=scene.correction();
    const QString source=scene.sourceClip>=0 && scene.clips?(*scene.clips)[scene.sourceClip].name:uiText("Pose keys");
    if(scene.compareOriginal)return uiText("Viewing source: %1 | Recording OFF - turn off Compare to edit").arg(source);
    if(layer.present)return uiText("%1%2 | %3 | Source: %4").arg(layer.enabled && layer.editing?uiText("Record: "):uiText("Locked: "),layer.name,
        layer.world?uiText("World offsets"):uiText("Local offsets"),source);
    if(scene.sourceClip>=0)return uiText("Source: %1 (read-only) | First edit creates a correction layer").arg(source);
    return uiText("Record: whole-pose keys | TRS + pins + morphs");
}
QWidget *vectorRow(std::array<QDoubleSpinBox *, 3> &fields, QWidget *parent, const QString &prefix) {
    auto row = new QWidget(parent);
    auto l = new QHBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    const QString axes = "XYZ";
    for (int i = 0; i < 3; ++i) {
        auto box = new QDoubleSpinBox(row);
        box->setObjectName(prefix + axes[i]);
        box->setRange(-100000, 100000);
        box->setDecimals(3);
        box->setSingleStep(.01);
        box->setPrefix(axes[i] + uiText(" "));
        box->setKeyboardTracking(false);
        box->setMinimumWidth(70);
        box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        fields[i] = box;
        l->addWidget(box);
    }
    return row;
}
} // namespace
Window::Window() {
    setObjectName("JesterWindow");
    bindUiText(this,"windowTitle","Jester");
    auto available = QGuiApplication::primaryScreen()->availableGeometry();
    resize(std::min(1440, available.width()), std::min(940, available.height()));
    setMinimumSize(760, 420);
    setDockOptions(AllowTabbedDocks | AllowNestedDocks | AnimatedDocks);
    setAcceptDrops(true);
    viewport = new Viewport(&document(), this);
    cameraDocumentId=document().id;
    connect(&workspace, &Workspace::activeChanged, this, [this] {
        if(!cameraDocumentId.isEmpty()) sceneCameras[cameraDocumentId]=viewport->camera();
        cameraDocumentId=document().id;
        viewport->setDocument(&document());
        if(sceneCameras.contains(cameraDocumentId)) viewport->setCamera(sceneCameras.value(cameraDocumentId));
        else {viewport->fit();sceneCameras[cameraDocumentId]=viewport->camera();}
        if (timeline)
            timeline->setDocument(&document());
    });
    connect(&workspace,&Workspace::changed,this,[this]{analysisJobs.poll();});
    auto centralWorkspace = new QWidget(this);
    auto workspaceLayout = new QVBoxLayout(centralWorkspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    sceneTabs = new QTabBar(centralWorkspace);
    sceneTabs->setObjectName("sceneTabs");
    sceneTabs->setTabsClosable(true);
    sceneTabs->setExpanding(false);
    sceneTabs->setDrawBase(false);
    workspaceLayout->addWidget(sceneTabs);
    workspaceLayout->addWidget(viewport, 1);
    setCentralWidget(centralWorkspace);
    auto skeleton = translatedWidget<QDockWidget>("Outliner", this);
    skeleton->setObjectName("skeletonDock");
    tree = new QTreeWidget(skeleton);
    tree->setObjectName("skeletonTree");
    tree->setHeaderHidden(true);
    tree->setMinimumWidth(190);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    skeleton->setWidget(tree);
    addDockWidget(Qt::RightDockWidgetArea, skeleton);
    connect(tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (refreshing)
            return;
        commitPendingEditor();
        QSet<int> nodes;
        for (auto item : tree->selectedItems())
            nodes.insert(item->data(0, Qt::UserRole).toInt());
        int primary = tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toInt() : -1;
        document().setSelection(nodes, primary);
    });
    buildInspector();
    buildTimeline();
    buildActions();
    buildHelpers();
    buildSceneWorkspace();
    buildAdvancedPanels();
    modernizeLayout();
    buildMotionPanel();
    new NotesController(this);
    installSceneSettings(*this);
    auto *preferences=new QAction(this);preferences->setObjectName("preferencesAction");bindUiText(preferences,"text","Preferences");preferences->setShortcut(QKeySequence("Ctrl+,"));
    menuBar()->addAction(preferences);addAction(preferences);connect(preferences,&QAction::triggered,this,[this]{showPreferences(*this);});
    connect(&PreferencesService::instance(),&PreferencesService::changed,this,[this] {ActionRegistry(*this).apply(PreferencesService::instance().current());fitCompactLayout();viewport->update();});
    statusBar()->setSizeGripEnabled(true);
    frameInfo = new QLabel(this);
    statusBar()->addPermanentWidget(frameInfo);
    connect(&workspace, &Workspace::sceneReplaced, this, &Window::rebuildTree);
    connect(&workspace, &Workspace::changed, this, &Window::refresh);
    connect(&workspace, &Workspace::selectionChanged, this, [this] {
        refresh();
        if (compactLayout && !document().navigatingAnnotation)
            findChild<QDockWidget *>("inspectorDock")->raise();
        tree->scrollToItem(tree->currentItem());
    });
    connect(&workspace, &Workspace::feedback, this, [this](QString s) { report(s); });
    setProperty("jesterDefaultDockState",saveState());
    QSettings settings;
    lastDirectory = settings.value("lastDirectory", QDir::homePath()).toString();
    if (!settings.value("geometry").toByteArray().isEmpty())
        restoreGeometry(settings.value("geometry").toByteArray());
    if (!settings.value("layoutV3").toByteArray().isEmpty())
        restoreState(settings.value("layoutV3").toByteArray());
    rebuildTree();
    refresh();
    viewport->fit();
    report(QString("Empty scene. Choose Scene (Ctrl+Shift+O), open a character or create a mannequin."));
    buildRecovery();
    buildAutomation();
    ActionRegistry(*this).apply(PreferencesService::instance().current());
    applyEditorAppearance(*qApp,PreferencesService::instance().current());
    connect(&LocalizationService::instance(),&LocalizationService::languageChanged,this,[this] {
        const auto restore=preserveUiDrafts(*this);
        // Refresh translated dynamic summaries without committing numeric or
        // layer-name fields back into the document.
        std::vector<std::unique_ptr<QSignalBlocker>> blocked;
        for(auto *widget:findChildren<QWidget*>())blocked.push_back(std::make_unique<QSignalBlocker>(widget));
        refresh();restore();viewport->update();timeline->update();
    });
}
void Window::buildInspector() {
    auto dock = translatedWidget<QDockWidget>("Inspector", this);
    dock->setObjectName("inspectorDock");
    auto scroll = new QScrollArea(dock);
    scroll->setObjectName("inspectorScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto body = new QWidget;
    auto layout = new QVBoxLayout(body);
    selectionLabel = new QLabel;
    selectionLabel->setWordWrap(true);
    layout->addWidget(selectionLabel);
    mirrorLabel = new QLabel(body);
    mirrorLabel->setObjectName("mirrorPartnerLabel");
    mirrorLabel->setWordWrap(true);
    layout->addWidget(mirrorLabel);
    auto mirrorTools = new QHBoxLayout;
    auto opposite = translatedWidget<QPushButton>("Opposite", body);
    opposite->setObjectName("selectOppositeButton");
    auto copyOpposite = translatedWidget<QPushButton>("Copy", body);
    copyOpposite->setObjectName("copyOppositeButton");
    bindUiText(copyOpposite,"toolTip","Copy selected FK bone / IK chain to its opposite, preserving the source");
    auto configure = translatedWidget<QPushButton>("Pairs...", body);
    configure->setObjectName("configureMirrorPairsButton");
    mirrorTools->addWidget(opposite);
    mirrorTools->addWidget(copyOpposite);
    mirrorTools->addWidget(configure);
    layout->addLayout(mirrorTools);
    connect(configure, &QPushButton::clicked, this, &Window::mirrorPairsDialog);
    connect(opposite, &QPushButton::clicked, this,
            [this] { document().select(document().scene.mirrorPartner(document().selected)); });
    connect(copyOpposite, &QPushButton::clicked, this, [this] {
        int n = document().selected;
        document().edit("Copy selected to opposite", [&](Scene &s) {
            std::vector<int> nodes{n};
            if (s.isEffector(n)) {
                auto chain = s.ikChain(n);
                nodes = {chain.root, chain.mid, n};
            }
            s.copyToOpposite(nodes);
        });
    });
    auto transform = translatedWidget<QGroupBox>("Stored local TRS (FK)", body);
    transform->setObjectName("localTransformGroup");
    auto f = new QFormLayout(transform);
    f->setRowWrapPolicy(QFormLayout::WrapAllRows);
    addTranslatedRow(f,"Position", vectorRow(position, transform, "position"));
    addTranslatedRow(f,"Rotation", vectorRow(rotation, transform, "rotation"));
    for (int i = 0; i < 3; ++i) {
        rotation[i]->setSingleStep(1);
        connect(position[i], &QDoubleSpinBox::editingFinished, this, [this] { readTransform(false); });
        connect(rotation[i], &QDoubleSpinBox::editingFinished, this, [this] { readTransform(true); });
    }
    layout->addWidget(transform);
    auto ik = translatedWidget<QGroupBox>("IK", body);
    ikBox = ik;
    auto form = new QFormLayout(ik);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    pin = translatedWidget<QCheckBox>("Pin in world space", ik);
    pin->setObjectName("pinCheck");
    form->addRow(pin);
    addTranslatedRow(form,"Target", vectorRow(target, ik, "target"));
    addTranslatedRow(form,"Pole", vectorRow(pole, ik, "pole"));
    weight = new QDoubleSpinBox(ik);
    weight->setObjectName("pinWeight");
    weight->setRange(0, 1);
    weight->setSingleStep(.1);
    weight->setDecimals(2);
    weight->setKeyboardTracking(false);
    addTranslatedRow(form,"Pin weight", weight);
    connect(pin, &QCheckBox::toggled, this, [this] {
        if (!refreshing)
            document().togglePin();
    });
    connect(weight, &QDoubleSpinBox::editingFinished, this, [this] {
        if (!refreshing && document().scene.pins.contains(document().selected))
            document().edit("Change pin weight", [this](Scene &s) {
                s.pins[document().selected].weight = float(weight->value());
                s.solvePins();
            });
    });
    for (int i = 0; i < 3; ++i) {
        connect(target[i], &QDoubleSpinBox::editingFinished, this, [this] { readIK(false); });
        connect(pole[i], &QDoubleSpinBox::editingFinished, this, [this] { readIK(true); });
    }
    layout->addWidget(ik);
    auto pins = translatedWidget<QPushButton>("Pin both feet", body);
    pins->setObjectName("pinFeetButton");
    bindUiText(pins,"toolTip","Find foot/ankle joints by name and keep both at their current world positions.");
    connect(pins, &QPushButton::clicked, this, [this] { document().pinFeet(); });
    layout->insertWidget(1, pins);
    auto reset = translatedWidget<QPushButton>("Reset pose", body);
    reset->setObjectName("resetPoseButton");
    connect(reset, &QPushButton::clicked, this, [this] { document().resetPose(); });
    layout->addWidget(reset);
    auto tip =
        translatedWidget<QLabel>("1  Move to a frame\n2  Drag a hand, foot or rotation ring\n3  Every pose edit creates a "
                   "key\n4  Space to play · Ctrl+S to save\n\nGreen controls are pinned.\nPurple diamonds "
                   "steer elbows and knees.\nUnreachable IK goals clamp to limb length.",
                   body);
    tip->setWordWrap(true);
    tip->setStyleSheet("padding-top: 8px;");
    tip->hide();
    layout->addStretch();
    scroll->setWidget(body);
    dock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->setMinimumWidth(280);
    resizeDocks({dock}, {320}, Qt::Horizontal);
}
void Window::buildTimeline() {
    auto dock = translatedWidget<QDockWidget>("Animation", this);
    dock->setObjectName("animationDock");
    auto body = new QWidget;
    auto l = new QVBoxLayout(body);
    l->setContentsMargins(6, 3, 6, 3);
    l->setSpacing(3);
    auto controls = new QHBoxLayout;
    auto previous = translatedWidget<QPushButton>("|◀", body);
    previous->setObjectName("previousKeyButton");
    bindUiText(previous,"toolTip","Previous key (,)");
    connect(previous, &QPushButton::clicked, this, [this] {
        document().play(false);
        const auto nav = timeline->navigationKeys();
        auto it = std::lower_bound(nav.cbegin(), nav.cend(), document().frame);
        if (it != nav.cbegin()) {
            --it;
            document().seek(*it);
        } else
            document().seek(document().scene.first);
    });
    controls->addWidget(previous);
    auto play = translatedWidget<QPushButton>("Play / Pause", body);
    play->setObjectName("playButton");
    bindUiText(play,"toolTip","Space");
    connect(play, &QPushButton::clicked, this, [this] { document().play(!document().playing); });
    controls->addWidget(play);
    auto next = translatedWidget<QPushButton>("▶|", body);
    next->setObjectName("nextKeyButton");
    bindUiText(next,"toolTip","Next key (.)");
    connect(next, &QPushButton::clicked, this, [this] {
        document().play(false);
        const auto nav = timeline->navigationKeys();
        auto it = std::upper_bound(nav.cbegin(), nav.cend(), document().frame);
        document().seek(it == nav.cend() ? document().scene.last : *it);
    });
    controls->addWidget(next);
    auto loop = translatedWidget<QCheckBox>("Loop", body);
    loop->setObjectName("loopCheck");
    loop->setChecked(true);
    connect(loop, &QCheckBox::toggled, this, [this](bool on) { document().loop = on; });
    controls->addWidget(loop);
    auto spin = [&](QString text, QString name, int min, int max) {
        controls->addWidget(translatedLabel(text, body));
        auto box = new QSpinBox(body);
        box->setObjectName(name);
        box->setRange(min, max);
        box->setKeyboardTracking(false);
        controls->addWidget(box);
        return box;
    };
    current = spin("", "currentFrame", 0, 100000);
    first = spin("In", "firstFrame", 0, 100000);
    last = spin("Out", "lastFrame", 0, 100000);
    fps = spin("fps", "fps", 1, 240);
    controls->addStretch();
    auto insert = translatedWidget<QPushButton>("◆ Insert key  I", body);
    insert->setObjectName("insertKeyButton");
    connect(insert, &QPushButton::clicked, this, [this] { timeline->insertKey(); });
    controls->addWidget(insert);
    auto contextButton = translatedWidget<QPushButton>("Notes",body);
    contextButton->setObjectName("motionContextButton");
    bindUiText(contextButton,"toolTip","Frame and interval intentions, observations and hypotheses");
    connect(contextButton,&QPushButton::clicked,this,[this]{timeline->editContext();});
    controls->addWidget(contextButton);
    auto motionTools=new QToolButton(body);
    motionTools->setObjectName("motionToolsButton");
    bindUiText(motionTools,"text","Motion tools");motionTools->setPopupMode(QToolButton::InstantPopup);
    auto motionMenu=new QMenu(motionTools);
    addTranslatedAction(motionMenu,"Graph Editor",this,&Window::showGraphEditor)->setObjectName("graphEditorAction");
    addTranslatedAction(motionMenu,"Motion Paths / measurements",this,&Window::showMotionAnalysis)->setObjectName("motionAnalysisAction");
    addTranslatedAction(motionMenu,"Humanoid retarget",this,&Window::showRetarget)->setObjectName("retargetAction");
    motionTools->setMenu(motionMenu);controls->addWidget(motionTools);
    l->addLayout(controls);
    recordingLabel = new QLabel(body);
    recordingLabel->setObjectName("timelineRecordingContext");
    recordingLabel->setWordWrap(false);
    recordingLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    recordingLabel->setText(recordingContextLabel(document()));
    auto options = new QHBoxLayout;
    auto selectedOnly = translatedWidget<QCheckBox>("Selection", body);
    selectedOnly->setObjectName("timelineSelectedOnly");
    selectedOnly->setChecked(true);
    bindUiText(selectedOnly,"toolTip","Only show selected bones/controllers. Shift-click to select several.");
    auto symmetry = translatedWidget<QCheckBox>("Symmetry X", body);
    symmetry->setObjectName("timelineSymmetry");
    bindUiText(symmetry,"toolTip","Mirror the edited bone/IK chain to its assigned partner; one Undo. Missing pairs "
                         "leave the other side unchanged and are reported.");
    auto compare = translatedWidget<QCheckBox>("Compare", body);
    compare->setObjectName("timelineCompare");
    bindUiText(compare,"toolTip","Compare source: show the uncorrected animation, with recording disabled");
    options->addWidget(recordingLabel, 1);
    options->addWidget(selectedOnly);
    options->addWidget(symmetry);
    options->addWidget(compare);
    l->addLayout(options);
    connect(symmetry, &QCheckBox::toggled, this, [this](bool value) { document().setSymmetry(value); });
    connect(compare, &QCheckBox::toggled, this, [this](bool value) {
        applyOperation("motion_correction", {{"operation", "compare"}, {"enabled", value}}, "Compare source");
    });
    timeline = new Timeline(&document(), body);
    connect(timeline,&Timeline::graphRequested,this,&Window::showGraphEditor);
    connect(selectedOnly, &QCheckBox::toggled, timeline, &Timeline::setOnlySelected);
    connect(timeline, &Timeline::scopeChanged, this, [insert](const QString &scope) {
        insert->setToolTip(uiText("Insert key (I): ") + scope);
        insert->setAccessibleName("Insert key: " + scope);
    });
    l->addWidget(timeline);
    dock->setWidget(body);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    connect(current, &QSpinBox::valueChanged, this, [this](int f) {
        if (!refreshing) {
            document().play(false);
            document().seek(f);
        }
    });
    auto range = [this] {
        if (refreshing)
            return;
        int a = first->value(), b = last->value(), rate = fps->value();
        if (a == document().scene.first && b == document().scene.last && rate == document().scene.fps)
            return;
        if (a > b) {
            report(QString("In frame must be at or before Out frame."));
            refresh();
            return;
        }
        document().edit(
            "Change clip range / FPS",
            [&](Scene &s) {
                s.first = a;
                s.last = b;
                s.fps = rate;
                document().frame = std::clamp(document().frame, a, b);
                s.evaluate(float(document().frame));
            },
            false);
    };
    connect(first, &QSpinBox::editingFinished, this, range);
    connect(last, &QSpinBox::editingFinished, this, range);
    connect(fps, &QSpinBox::editingFinished, this, range);
}
void Window::buildActions() {
    auto file = addTranslatedMenu(menuBar(),"&File");
    auto edit = addTranslatedMenu(menuBar(),"&Edit");
    auto view = addTranslatedMenu(menuBar(),"&View");
    auto help = addTranslatedMenu(menuBar(),"&Help");
    auto toolbar = addToolBar(uiText("Main"));
    bindUiText(toolbar,"windowTitle","Main");
    toolbar->setObjectName("mainToolbar");
    toolbar->setMovable(false);
    auto add = [&](QMenu *menu, QString text, QKeySequence shortcut, auto callback) {
        auto a = addTranslatedAction(menu,text);
        if (!shortcut.isEmpty())
            a->setShortcut(shortcut);
        connect(a, &QAction::triggered, this, callback);
        return a;
    };
    auto fresh = add(file, "New empty scene", QKeySequence::New, [this] { newEmptyScene(); });
    fresh->setObjectName("newAction");
    toolbar->addAction(fresh);
    auto openAction = add(file, "Open character / project…", QKeySequence::Open, [this] { chooseOpen(); });
    openAction->setObjectName("openAction");
    toolbar->addAction(openAction);
    auto saveAction = add(file, "Save project", QKeySequence::Save, [this] { save(); });
    saveAction->setObjectName("saveAction");
    toolbar->addAction(saveAction);
    add(file, "Save project as…", QKeySequence::SaveAs, [this] { save(true); });
    auto exportAction =
        add(file, "Export animation…", QKeySequence("Ctrl+Shift+E"), [this] { chooseExport(); });
    exportAction->setObjectName("exportAction");
    toolbar->addAction(exportAction);
    file->addSeparator();
    add(file, "New mannequin", {}, [this] { newCharacter(); });
    add(file, "Open animated demo", {}, [this] { newCharacter(true); });
    file->addSeparator();
    add(file, "Quit", QKeySequence::Quit, [this] { close(); });

    auto undo = workspace.history.createUndoAction(this, "Undo");
    undo->setObjectName("undo");undo->setProperty("jesterSourceText","Undo");
    undo->setShortcut(QKeySequence::Undo);
    edit->addAction(undo);
    auto redo = workspace.history.createRedoAction(this, "Redo");
    redo->setObjectName("redo");redo->setProperty("jesterSourceText","Redo");
    redo->setShortcuts({QKeySequence::Redo, QKeySequence("Ctrl+Shift+Z")});
    edit->addAction(redo);
    toolbar->addSeparator();
    toolbar->addAction(undo);
    toolbar->addAction(redo);
    auto historyText=[this,undo,redo] {
        undo->setText(workspace.history.undoText().isEmpty()?uiText("Undo"):uiText("Undo %1").arg(translatedDiagnostic(workspace.history.undoText())));
        redo->setText(workspace.history.redoText().isEmpty()?uiText("Redo"):uiText("Redo %1").arg(translatedDiagnostic(workspace.history.redoText())));
    };
    connect(&workspace.history,&QUndoGroup::undoTextChanged,this,historyText);
    connect(&workspace.history,&QUndoGroup::redoTextChanged,this,historyText);
    connect(&LocalizationService::instance(),&LocalizationService::languageChanged,this,historyText);historyText();
    edit->addSeparator();
    add(edit, "Insert key in recording target", QKeySequence("I"), [this] { timeline->insertKey(); });
    add(edit, "Pin / release selected", QKeySequence("P"), [this] { document().togglePin(); });
    add(edit, "Pin both feet", {}, [this] { document().pinFeet(); });
    add(edit, "Reset pose", {}, [this] { document().resetPose(); });
    toolbar->addSeparator();
    moveAction = add(edit, "Move / IK", QKeySequence("W"), [this] {
        viewport->tool = Viewport::Move;
        refresh();
    });
    moveAction->setCheckable(true);
    moveAction->setObjectName("moveAction");
    toolbar->addAction(moveAction);
    rotateAction = add(edit, "Rotate / FK", QKeySequence("E"), [this] {
        viewport->tool = Viewport::Rotate;
        refresh();
    });
    rotateAction->setObjectName("rotateAction");
    rotateAction->setCheckable(true);
    toolbar->addAction(rotateAction);
    localAction = add(edit, "Local axes", QKeySequence("Q"), [this] {
        viewport->local = !viewport->local;
        refresh();
    });
    localAction->setCheckable(true);
    toolbar->addAction(localAction);
    playAction =
        add(edit, "Play / Pause", QKeySequence("Space"), [this] { document().play(!document().playing); });
    playAction->setCheckable(true);
    playAction->setObjectName("playAction");
    add(edit, "Previous frame", QKeySequence(Qt::Key_Left), [this] {
        document().play(false);
        document().seek(document().frame - 1);
    });
    add(edit, "Next frame", QKeySequence(Qt::Key_Right), [this] {
        document().play(false);
        document().seek(document().frame + 1);
    });
    add(edit, "Previous key", QKeySequence(","),
        [this] { findChild<QPushButton *>("previousKeyButton")->click(); });
    add(edit, "Next key", QKeySequence("."), [this] { findChild<QPushButton *>("nextKeyButton")->click(); });
    add(view, "Frame character", QKeySequence("F"), [this] { viewport->frameCharacter(); });
    add(view, "Perspective", QKeySequence("0"), [this] { viewport->viewPreset(0); });
    add(view, "Front", QKeySequence("1"), [this] { viewport->viewPreset(1); });
    add(view, "Side", QKeySequence("3"), [this] { viewport->viewPreset(2); });
    add(view, "Top", QKeySequence("7"), [this] { viewport->viewPreset(3); });
    view->addSeparator();
    auto check = [&](QString text, bool *member, bool initial) {
        auto a = addTranslatedAction(view,text);
        a->setCheckable(true);
        a->setChecked(initial);
        connect(a, &QAction::toggled, this, [this, member](bool on) {
            *member = on;
            viewport->update();
        });
    };
    check("Show mesh", &viewport->showMesh, true);
    check("Show skeleton", &viewport->showBones, true);
    check("Adjacent key poses", &viewport->showGhost, false);
    check("Selected joint trajectory", &viewport->showTrajectory, false);
    view->addSeparator();
    for (auto dock : findChildren<QDockWidget *>())
        view->addAction(dock->toggleViewAction());
    add(view, "Restore default layout", {}, [this] { resetLayout(); });
    add(help, "Quick start", QKeySequence::HelpContents, [this] {
        showUiMessage(QMessageBox::Information,this,[&] {return uiText("Jester · Quick start");},[&] {return translatedDiagnostic(uiText("Open a glTF/GLB character or use the built-in mannequin.\n\n1. Select Hips and click Pin both "
            "feet. Drag the pelvis down.\n2. Drag a hand or foot for IK. Use the purple pole for bend "
            "direction.\n3. Choose a new frame and pose again. Every completed edit creates a full pose "
            "key.\n4. Space plays the clip. I inserts a key without changing the pose.\n5. Ctrl+S saves a "
            "self-contained .jst project.\n6. Ctrl+Shift+E exports a baked .glb or .gltf "
            "animation.\n\nW / E: move / rotate · Q: world / local axes\nRMB orbit · MMB pan · wheel zoom · "
            "F frame character\nTimeline: drag diamonds to move; Ctrl+drag to copy; Delete removes a "
            "selected key.\nCtrl+Z / Ctrl+Shift+Z: undo / redo. Escape cancels a viewport drag.\n\nIK is "
            "analytic two-bone IK. Unreachable targets clamp to limb length.\n\nFBX, GLB and glTF retain "
            "textures and blend shapes. Physics panel: fit bodies, preview simulation or a gravity-based "
            "flight, then open the result as a scene."));});
    });
    add(help, "About Jester", {}, [this] {
        const auto backend=viewport->renderingStats()["rhi"].toString().toUpper();
        showUiMessage(QMessageBox::Information,this,[&] {return uiText("Jester");},[&] {return translatedDiagnostic(uiText("Jester %1 | %2\nCharacter animation editor\nC++20 / Qt 6 Widgets / O3DE Atom (%3)\n\nApplication code: MIT\nThird-party licenses: see THIRD_PARTY.md.\nExperimental physics: MuJoCo 3.3.7. FBX: ufbx.").arg(TRICKSTER_VERSION,TRICKSTER_BUILD_ID,backend));});
    });
}
void Window::rebuildTree() {
    refreshing = true;
    tree->clear();
    std::vector<QTreeWidgetItem *> items;
    for (int i = 0; i < int(document().scene.nodes.size()); ++i) {
        auto item = new QTreeWidgetItem;
        item->setText(0, document().scene.nodes[i].name);
        item->setData(0, Qt::UserRole, i);
        item->setIcon(0, icon(document().scene.nodes[i].joint ? Glyph::Bones : Glyph::Mesh));
        item->setToolTip(0, uiText("%1 · node %2").arg(document().scene.nodes[i].name).arg(i));
        items.push_back(item);
    }
    for (int i = 0; i < int(items.size()); ++i) {
        int parent = document().scene.nodes[i].parent;
        if (parent >= 0)
            items[parent]->addChild(items[i]);
        else
            tree->addTopLevelItem(items[i]);
    }
    tree->expandToDepth(3);
    refreshing = false;
}
void Window::refresh() {
    QElapsedTimer refreshTimer;refreshTimer.start();auto refreshGuard=qScopeGuard([&]{lastPanelRefreshMs=refreshTimer.nsecsElapsed()/1e6;});
    if (refreshing)
        return;
    refreshing = true;
    auto &s = document().scene;
    const bool hasCharacter = !s.nodes.empty();
    const bool canRecord = hasCharacter && !s.compareOriginal &&
                           (!s.correction().present || (s.correction().enabled && s.correction().editing));
    emptyPanel->setVisible(!hasCharacter);
    findChild<QMenu *>("poseMenu")->setEnabled(hasCharacter);
    positionEmptyPanel();
    selectionLabel->parentWidget()->setEnabled(hasCharacter);
    findChild<QGroupBox *>("localTransformGroup")
        ->setEnabled(hasCharacter && document().selection().size() == 1 && document().selected >= 0 &&
                     !s.compareOriginal &&
                     (!s.correction().present || (s.correction().enabled && s.correction().editing)));
    int partner = s.mirrorPartner(document().selected);
    if (mirrorLabel)
        mirrorLabel->setText(partner >= 0
                                 ? s.nodes[document().selected].name + " <-> " + s.nodes[partner].name
                             : document().selected >= 0 ? uiText("Mirror partner not assigned - Pairs...")
                                                        : uiText("Select a bone to inspect its mirror pair"));
    findChild<QPushButton *>("selectOppositeButton")->setEnabled(partner >= 0);
    findChild<QPushButton *>("copyOppositeButton")->setEnabled(partner >= 0 && canRecord);
    bool feet = false;
    for (int n = 0; n < int(s.nodes.size()); n++)
        if (s.isFoot(n) && s.isEffector(n))
            feet = true;
    findChild<QPushButton *>("pinFeetButton")->setEnabled(feet && canRecord);
    if (recordingLabel) {
        recordingLabel->setText(recordingContextLabel(document()));
        recordingLabel->setToolTip(recordingContextLabel(document()));
    }
    if (auto box = findChild<QCheckBox *>("timelineSymmetry")) {
        QSignalBlocker b(box);
        box->setChecked(document().symmetryEditing);
    }
    if (auto box = findChild<QCheckBox *>("timelineCompare")) {
        QSignalBlocker b(box);
        box->setChecked(s.compareOriginal);
        box->setEnabled(s.sourceClip >= 0 || s.correction().present);
    }
    if (symmetryAction) {
        QSignalBlocker b(symmetryAction);
        symmetryAction->setChecked(document().symmetryEditing);
    }
    findChild<QAction *>("exportAction")->setEnabled(hasCharacter);
    findChild<QPushButton *>("insertKeyButton")->setEnabled(canRecord);
    playAction->setEnabled(hasCharacter);
    findChild<QPushButton *>("playButton")->setEnabled(hasCharacter);
    if (!hasCharacter || document().selected < 0) {
        selectionLabel->setText(hasCharacter ? uiText("No selection") : uiText("No character selected"));
        tree->setCurrentItem(nullptr);
        ikBox->hide();
        limitEnabled->setChecked(false);
        pin->setChecked(false);
        for (int i = 0; i < 3; ++i) {
            position[i]->setValue(0);
            rotation[i]->setValue(0);
        }
    }
    setWindowTitle(uiText("%1%2 — Jester")
                       .arg(document().path.isEmpty() ? s.name : QFileInfo(document().path).fileName(),
                            document().dirty() ? " *" : ""));
    current->setRange(s.first, s.last);
    current->setValue(document().frame);
    first->setValue(s.first);
    last->setValue(s.last);
    fps->setValue(s.fps);
    frameInfo->setText(s.compareOriginal ? uiText("Source / compare")
                       : s.correction().present
                           ? (s.correction().editing && s.correction().enabled ? uiText("REC: ") : uiText("Locked: ")) +
                                 s.correction().name
                       : s.sourceClip >= 0 ? uiText("Source / auto correction")
                                           : uiText("Whole-pose keys"));
    if (document().selected >= 0 && document().selected < int(s.nodes.size())) {
        int n = document().selected;
        bool contact = false;
        for (const auto &c : s.contacts)
            if (c.node == n && document().frame >= c.first && document().frame <= c.last)
                contact = true;
        const auto &t = s.pose[n];
        auto angles = (viewport->local ? t.r : s.globalRotation(n, s.pose)).toEulerAngles();
        auto displayedPosition = viewport->local ? t.t : s.positions(s.pose)[n];
        findChild<QGroupBox *>("localTransformGroup")
            ->setTitle(viewport->local ? uiText("Local transform (FK)") : uiText("World transform (FK)"));
        findChild<QGroupBox *>("localTransformGroup")
            ->setToolTip(uiText("Numeric fields use the gizmo coordinate space. Select one bone for numeric "
                         "editing; drag the gizmo to transform a group."));
        selectionLabel->setText((document().selection().size() > 1
                                     ? uiText("%1 selected / ").arg(document().selection().size())
                                     : QString()) +
                                s.nodes[n].name + " / " + (s.isEffector(n) ? uiText("Move: IK chain") : uiText("Move: FK")) +
                                uiText(" / Gizmo: ") + (viewport->local ? uiText("Local") : uiText("World")));
        selectionLabel->setToolTip(contact           ? uiText("Planted contact")
                                   : s.isEffector(n) ? uiText("End effector · drag for IK")
                                                     : uiText("Local FK transform"));
        float step = std::pow(10.f, std::floor(std::log10(viewport->characterHeight())) - 2);
        for (int i = 0; i < 3; ++i) {
            position[i]->setValue(displayedPosition[i]);
            rotation[i]->setValue(angles[i]);
            position[i]->setSingleStep(step);
        }
        qobject_cast<QGroupBox *>(ikBox)->setTitle(document().selection().size() > 1 ? uiText("IK: active controller")
                                                                                     : "IK");
        ikBox->setVisible(s.isEffector(n));
        ikBox->setEnabled(canRecord && s.isEffector(n) && s.ikDiagnostic(n).isEmpty());
        ikBox->setToolTip(s.ikDiagnostic(n));
        if (s.profile.configured && !s.isEffector(n))
            bindToolTip(selectionLabel,"FK joint. Assign an IK chain in Motion → Character profile to use it as an effector.");
        pin->setChecked(s.pins.contains(n));
        limitEnabled->setChecked(s.rotationLimits.contains(n) || s.jointLimits.contains(n));
        limitAngle->setEnabled(s.rotationLimits.contains(n) && !s.jointLimits.contains(n));
        limitAngle->setValue(s.rotationLimits.value(n, 150));
        weight->setEnabled(s.pins.contains(n));
        if (s.isEffector(n)) {
            auto p = s.positions(s.pose)[n];
            auto pl = s.pins.contains(n) ? s.pins[n].pole : s.defaultPole(n);
            auto goal = s.pins.contains(n) ? s.pins[n].target : p;
            for (int i = 0; i < 3; ++i) {
                target[i]->setValue(goal[i]);
                pole[i]->setValue(pl[i]);
                target[i]->setSingleStep(step);
                pole[i]->setSingleStep(step);
            }
            weight->setValue(s.pins.contains(n) ? s.pins[n].weight : 1);
        }
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            int idx = (*it)->data(0, Qt::UserRole).toInt();
            bool activeContact = false;
            for (const auto &c : s.contacts)
                if (c.node == idx && document().frame >= c.first && document().frame <= c.last)
                    activeContact = true;
            (*it)->setText(0, s.nodes[idx].name + (s.pins.contains(idx) ? uiText("  [pin]")
                                                   : activeContact      ? uiText("  [contact]")
                                                                        : ""));
            (*it)->setSelected(document().selection().contains(idx));
            if (idx == n && tree->currentItem() != *it)
                tree->setCurrentItem(*it, 0, QItemSelectionModel::NoUpdate);
            ++it;
        }
    }
    moveAction->setChecked(viewport->tool == Viewport::Move);
    rotateAction->setChecked(viewport->tool == Viewport::Rotate);
    localAction->setChecked(viewport->local);
    playAction->setChecked(document().playing);
    assistAction->setChecked(s.bodyAssist);
    refreshSceneTabs();
    refreshPanels();
    refreshing = false;
    fitCompactLayout();
    viewport->update();
    timeline->update();
}
bool Window::applyOperation(const QString &name, const QJsonObject &arguments, QString label) {
    try {
        EditService::execute(document(), name, arguments, label);
        return true;
    } catch (const std::exception &error) {
        report(translatedDiagnostic(QString::fromUtf8(error.what())));
        refresh();
        return false;
    }
}
void Window::readTransform(bool rot) {
    if (refreshing || document().selected < 0)
        return;
    QVector3D v;
    for (int i = 0; i < 3; ++i)
        v[i] = float((rot ? rotation[i] : position[i])->value());
    auto &s = document().scene;
    int n = document().selected;
    auto old = rot ? (viewport->local ? s.pose[n].r : s.globalRotation(n, s.pose)).toEulerAngles()
               : viewport->local ? s.pose[n].t
                                 : s.positions(s.pose)[n];
    if ((old - v).length() < .0009f)
        return;
    applyOperation("pose_transform",
                   {{"node", n},
                    {rot ? "rotation" : "position", QJsonArray{v.x(), v.y(), v.z()}},
                    {"space", viewport->local ? "local" : "world"},
                    {"rotationSpace", viewport->local ? "local" : "world"}},
                   rot ? "Set joint rotation" : "Set joint position");
}
void Window::readIK(bool changePole) {
    if (refreshing || !document().scene.isEffector(document().selected))
        return;
    QVector3D goal, pl;
    for (int i = 0; i < 3; ++i) {
        goal[i] = float(target[i]->value());
        pl[i] = float(pole[i]->value());
    }
    auto &s = document().scene;
    int n = document().selected;
    auto oldGoal = s.pins.contains(n) ? s.pins[n].target : s.positions(s.pose)[n];
    auto oldPole = s.pins.contains(n) ? s.pins[n].pole : s.defaultPole(n);
    if ((goal - oldGoal).length() < .0009f && (pl - oldPole).length() < .0009f)
        return;
    applyOperation("pose_ik",
                   {{"node", n},
                    {"target", QJsonArray{goal.x(), goal.y(), goal.z()}},
                    {"pole", QJsonArray{pl.x(), pl.y(), pl.z()}},
                    {"weight", weight->value()},
                    {"pin", s.pins.contains(n) || changePole}},
                   changePole ? "Set IK pole" : "Set IK target");
}
void Window::report(QString text, bool error) {
    auto *bar=statusBar();bar->setProperty("jesterStatusSource",text);
    if(!bar->property("jesterStatusBound").toBool()) {
        bar->setProperty("jesterStatusBound",true);
        connect(&LocalizationService::instance(),&LocalizationService::languageChanged,bar,[bar] {
            if(!bar->currentMessage().isEmpty())bar->showMessage(translatedDiagnostic(bar->property("jesterStatusSource").toString()),8000);
        });
    }
    bar->showMessage(translatedDiagnostic(text), error ? 15000 : 8000);
    if (error)
        showUiMessage(QMessageBox::Warning,this,[&] {return uiText("Jester");},[&] {return translatedDiagnostic(text);});
}
bool Window::mayClose() {
    if (!document().dirty())
        return true;
    auto result = showUiMessage(QMessageBox::Question,this,[&] {return uiText("Jester · Unsaved changes");},[&] {return translatedDiagnostic(uiText("Save changes to this animation before continuing?"));},QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,QMessageBox::Save);
    return result == QMessageBox::Discard || (result == QMessageBox::Save && save());
}
bool Window::save(bool as) {
    commitPendingEditor();
    QString path = document().path;
    if (path.isEmpty() || as) {
        QFileDialog dialog(this,uiText("Save Jester project"),
                           QDir(lastDirectory).filePath(suggestedProjectFileName(document().scene.name)),
                           uiText("Jester project (*.jst)"));
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setFileMode(QFileDialog::AnyFile);
        dialog.setDefaultSuffix("jst");
        if(dialog.exec()!=QDialog::Accepted)return false;
        QString error;
        path=normalizedProjectSavePath(dialog.selectedFiles().value(0),error);
        if(path.isEmpty()) {report(error,true);return false;}
    }
    for (int i = 0; i < int(workspace.documents.size()); ++i)
        if (i != workspace.activeIndex && !workspace.documents[i]->path.isEmpty() &&
            QFileInfo(workspace.documents[i]->path)
                    .absoluteFilePath()
                    .compare(QFileInfo(path).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
            report(QString("This project is open in another scene. Choose a different filename."), true);
            return false;
        }
    QString error;
    if (!saveProject(path, document().scene, error)) {
        report(error, true);
        return false;
    }
    document().path = path;
    lastDirectory = QFileInfo(path).absolutePath();
    document().undo.setClean();
    refresh();
    report(QString("Saved project: ") + path);
    return true;
}
bool Window::open(const QString &path) {
    if(!isSupportedSceneFile(path)) {
        report(QString("Unsupported scene file. Open a Jester project (.jst), FBX, GLB or glTF."),true);
        return false;
    }
    if (isNativeProjectFile(path)) {
        QString canonical = QFileInfo(path).canonicalFilePath();
        for (int i = 0; i < int(workspace.documents.size()); ++i) {
            QString existing = i == workspace.activeIndex ? document().path : workspace.documents[i]->path;
            if (!canonical.isEmpty() && !existing.isEmpty() &&
                QFileInfo(existing).canonicalFilePath() == canonical) {
                switchScene(i);
                report(QString("Project already open; switched to its scene."));
                return true;
            }
        }
    }

    Scene s;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = isNativeProjectFile(path)
                  ? loadProject(path, s, error)
                  : loadCharacter(path, s, error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        report(QString("Cannot open character/project: ") + error, true);
        return false;
    }
    bool project = isNativeProjectFile(path);
    lastDirectory = QFileInfo(path).absolutePath();
    auto warnings = s.warnings;
    appendScene(std::move(s), project ? path : QString());
    QSettings settings;
    auto recent = settings.value("recentScenes").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 12)
        recent.removeLast();
    settings.setValue("recentScenes", recent);
    report(QString("Opened ") + QFileInfo(path).fileName());
    if (!warnings.isEmpty())
        report(QString("Opened ") + QFileInfo(path).fileName() + " · Import notes: " + warnings.join(" · "));
    return true;
}
void Window::chooseOpen() {
    commitPendingEditor();
    document().play(false);
    auto path =
        QFileDialog::getOpenFileName(this, uiText("Open character or project"), lastDirectory,
                                     uiText("Characters and projects (*.glb *.gltf *.fbx *.jst);;glTF "
                                     "character (*.glb *.gltf *.fbx);;Jester project (*.jst)"));
    if (!path.isEmpty())
        open(path);
}
void Window::chooseExport() {
    commitPendingEditor();
    document().play(false);
    QFileDialog dialog(this,uiText("Export baked animation"),lastDirectory+"/animation.glb");
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilters({uiText("Binary glTF (*.glb)"),uiText("Embedded glTF (*.gltf)"),"FBX (*.fbx)"});
    dialog.setDefaultSuffix("glb");
    connect(&dialog, &QFileDialog::filterSelected, &dialog, [&dialog](QString filter) {
        QString suffix = filter.contains("*.fbx") ? "fbx" : filter.contains("*.gltf") ? "gltf" : "glb";
        dialog.setDefaultSuffix(suffix);
        auto files = dialog.selectedFiles();
        if (!files.isEmpty()) {
            auto file = QFileInfo(files.first());
            auto ext = file.suffix().toLower();
            if (ext == "glb" || ext == "gltf" || ext == "fbx")
                dialog.selectFile(file.completeBaseName() + "." + suffix);
        }
    });
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    QString path = dialog.selectedFiles().first(), filter = dialog.selectedNameFilter();
    if (!path.endsWith(".glb", Qt::CaseInsensitive) && !path.endsWith(".gltf", Qt::CaseInsensitive) &&
        !path.endsWith(".fbx", Qt::CaseInsensitive))
        path += filter.contains("*.fbx") ? ".fbx" : filter.contains("*.gltf") ? ".gltf" : ".glb";
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = exportAnimation(path, document().scene, error);
    QApplication::restoreOverrideCursor();
    if (ok) {
        lastDirectory = QFileInfo(path).absolutePath();
        report(QString("Exported %1–%2 at %3 fps: %4")
                   .arg(document().scene.first)
                   .arg(document().scene.last)
                   .arg(document().scene.fps)
                   .arg(path) +
               physicsExportStatus(document().scene));
    } else
        report(QString("Export failed: ") + error, true);
}
void Window::newCharacter(bool demo) {
    Scene s;
    if (demo) {
        Document temporary;
        temporary.demo();
        s = std::move(temporary.scene);
    } else
        s = Scene::mannequin();
    appendScene(std::move(s));
    report(demo ? QString("Animation demo opened in its own scene.")
                : QString("Mannequin ready. Drag a hand to start posing."));
}
void Window::saveDemo(const QString &directory) {
    QDir().mkpath(directory);
    document().demo();
    QString error;
    saveProject(directory + "/welcome.jst", document().scene, error);
    exportAnimation(directory + "/welcome.glb", document().scene, error);
    if (!error.isEmpty())
        qWarning("%s", qPrintable(error));
}
void Window::closeEvent(QCloseEvent *e) {
    commitPendingEditor();
    const int previous = workspace.activeIndex;
    for (int i = 0; i < int(workspace.documents.size()); ++i) {
        switchScene(i);
        if (!mayClose()) {
            switchScene(previous);
            e->ignore();
            return;
        }
    }
    for (const auto &d : workspace.documents)
        recovery.discard(d->id);
    QSettings s;
    s.setValue("geometry", saveGeometry());
    s.setValue("layoutV3", saveState());
    s.setValue("lastDirectory", lastDirectory);
    cancelPhysicsTask();
    e->accept();
}
void Window::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) {
        if (isSupportedSceneFile(e->mimeData()->urls().first().toLocalFile()))
            e->acceptProposedAction();
    }
}
void Window::dropEvent(QDropEvent *e) {
    if (e->mimeData()->hasUrls() && open(e->mimeData()->urls().first().toLocalFile()))
        e->acceptProposedAction();
}
} // namespace trickster
