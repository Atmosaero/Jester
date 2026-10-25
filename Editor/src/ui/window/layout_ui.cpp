#include "ui/widgets/icons.h"
#include "ui/window/window.h"
#include <QActionGroup>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
namespace trickster {
void Window::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    fitCompactLayout();
}
void Window::fitCompactLayout() {
    auto properties = findChild<QDockWidget *>("inspectorDock"),
         outliner = findChild<QDockWidget *>("skeletonDock");
    if (!properties || !outliner)
        return;
    if (height() < 600 && !compactLayout) {
        expandedLayout = saveState();
        compactLayout = true;
        tabifyDockWidget(outliner, properties);
        properties->show();
        properties->raise();
        if (auto animation = findChild<QDockWidget *>("animationDock"))
            resizeDocks({animation}, {210}, Qt::Vertical);
        QTimer::singleShot(0, this, [this] { fitCompactLayout(); });
    } else if (height() > 650 && compactLayout) {
        compactLayout = false;
        auto previous = expandedLayout;
        expandedLayout.clear();
        restoreState(previous);
    }
    const QMap<QString, Glyph> panels{{"Outliner", Glyph::Bones},
                                      {"Blend shapes", Glyph::Shapes},
                                      {"Physics", Glyph::Physics},
                                      {"Motion", Glyph::Key},
                                      {"Inspector", Glyph::Settings}, {"Notes", Glyph::Note}};
    static const QMap<QString, QIcon> panelIcons{{"Outliner", icon(Glyph::Bones)},
                                                 {"Blend shapes", icon(Glyph::Shapes)},
                                                 {"Physics", icon(Glyph::Physics)},
                                                 {"Motion", icon(Glyph::Key)},
                                                 {"Inspector", icon(Glyph::Settings)}, {"Notes", icon(Glyph::Note)}};
    for (auto dock : findChildren<QDockWidget *>())
        if (panels.contains(dock->windowTitle()) &&
            dock->windowIcon().cacheKey() != panelIcons[dock->windowTitle()].cacheKey())
            dock->setWindowIcon(panelIcons[dock->windowTitle()]);
    const QString compactStyle =
        "QTabBar::tab { min-width: 28px; max-width: 28px; padding: 4px; color: transparent; }";
    for (auto tabs : findChildren<QTabBar *>())
        if (tabs != sceneTabs) {
            QString style = compactLayout ? compactStyle : QString();
            if (tabs->styleSheet() != style)
                tabs->setStyleSheet(style);
            tabs->setElideMode(Qt::ElideRight);
            for (int i = 0; i < tabs->count(); i++) {
                auto title = tabs->tabText(i);
                title.remove('&');
                if (title.isEmpty())
                    title = tabs->tabToolTip(i);
                if (panels.contains(title)) {
                    if (tabs->tabIcon(i).cacheKey() != panelIcons[title].cacheKey())
                        tabs->setTabIcon(i, panelIcons[title]);
                    tabs->setTabToolTip(i, title);
                    tabs->setAccessibleTabName(i, title);
                }
            }
        }
}
void Window::resetLayout() {
    compactLayout = false;
    expandedLayout.clear();
    auto defaults=property("jesterDefaultDockState").toByteArray();
    if(!defaults.isEmpty()) {
        restoreState(defaults);
        fitCompactLayout();
        return;
    }
    auto outliner = findChild<QDockWidget *>("skeletonDock"),
         properties = findChild<QDockWidget *>("inspectorDock"),
         shapes = findChild<QDockWidget *>("shapesDock"), physics = findChild<QDockWidget *>("physicsDock"),
         animation = findChild<QDockWidget *>("animationDock");
    for (auto dock : {outliner, properties, shapes, physics, animation}) {
        dock->setFloating(false);
        dock->show();
        removeDockWidget(dock);
    }
    addDockWidget(Qt::RightDockWidgetArea, outliner);
    splitDockWidget(outliner, properties, Qt::Vertical);
    addDockWidget(Qt::RightDockWidgetArea, shapes);
    tabifyDockWidget(outliner, shapes);
    addDockWidget(Qt::RightDockWidgetArea, physics);
    tabifyDockWidget(outliner, physics);
    outliner->raise();
    addDockWidget(Qt::BottomDockWidgetArea, animation);
    for (auto dock : {outliner, properties, shapes, physics, animation})
        dock->show();
    if (auto motion = findChild<QDockWidget *>("motionDock")) {
        motion->setFloating(false);
        addDockWidget(Qt::RightDockWidgetArea, motion);
        tabifyDockWidget(outliner, motion);
        motion->show();
        outliner->raise();
    }
    outliner->raise();
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);
    resizeDocks({outliner, properties}, {280, 340}, Qt::Vertical);
    resizeDocks({properties}, {310}, Qt::Horizontal);
    resizeDocks({animation}, {210}, Qt::Vertical);
    fitCompactLayout();
}
void Window::modernizeLayout() {
    auto toolbar = findChild<QToolBar *>("mainToolbar");
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize({22, 22});
    toolbar->setStyleSheet("QToolBar { spacing: 2px; padding: 3px; border: 0; } QToolButton { min-width: "
                           "28px; min-height: 28px; border: 0; border-radius: 3px; } QToolButton:hover { "
                           "background: #41464d; } QToolButton:checked { background: #244d60; }");
    auto find = [&](QString prefix) -> QAction * {
        for (auto a : findChildren<QAction *>()) {
            auto text = a->text();
            text.remove('&');
            if (text.startsWith(prefix))
                return a;
        }
        return nullptr;
    };
    auto decorate = [&](QAction *a, Glyph glyph) {
        if (!a)
            return;
        a->setIcon(icon(glyph));
        auto tip = a->toolTip();
        if (tip.isEmpty() || tip == a->text())
            tip = a->text();
        if (!a->shortcut().isEmpty())
            tip += " (" + a->shortcut().toString(QKeySequence::NativeText) + ")";
        a->setToolTip(tip);
        a->setStatusTip(tip);
    };
    std::vector<std::pair<QString, Glyph>> mapping = {{"New empty scene", Glyph::New},
                                                      {"Choose scene", Glyph::Library},
                                                      {"Open character", Glyph::Open},
                                                      {"Save project", Glyph::Save},
                                                      {"Export animation", Glyph::Export},
                                                      {"Undo", Glyph::Undo},
                                                      {"Redo", Glyph::Redo},
                                                      {"Move / IK", Glyph::Move},
                                                      {"Rotate / FK", Glyph::Rotate},
                                                      {"Local axes", Glyph::Local},
                                                      {"Pin / release", Glyph::Pin},
                                                      {"Pin both feet", Glyph::Feet},
                                                      {"Flip entire pose", Glyph::Mirror},
                                                      {"Symmetric editing", Glyph::Mirror},
                                                      {"Adjacent key", Glyph::Ghost},
                                                      {"Selected joint trajectory", Glyph::Trajectory},
                                                      {"Show center of mass", Glyph::Mass},
                                                      {"Show mesh", Glyph::Mesh},
                                                      {"Show skeleton", Glyph::Bones},
                                                      {"Frame character", Glyph::Camera},
                                                      {"Insert key", Glyph::Key},
                                                      {"Body assists", Glyph::Bones},
                                                      {"Foot contact", Glyph::Feet},
                                                      {"Bake corrections", Glyph::Bake},
                                                      {"Ballistic COM", Glyph::Ballistic}};
    for (const auto &entry : mapping)
        decorate(find(entry.first), entry.second);
    toolbar->clear();
    auto group = [&](std::initializer_list<QString> prefixes) {
        if (!toolbar->actions().isEmpty())
            toolbar->addSeparator();
        for (const auto &prefix : prefixes)
            if (auto action = find(prefix))
                toolbar->addAction(action);
    };
    group({"New empty scene", "Choose scene", "Open character", "Save project", "Export animation"});
    group({"Undo", "Redo"});
    group({"Local axes", "Pin / release", "Pin both feet", "Symmetric editing"});
    group({"Adjacent key", "Selected joint trajectory", "Show center of mass"});
    group({"Body assists", "Foot contact", "Simulate interval", "Correct flight"});
    group({"Show mesh", "Show skeleton"});
    auto camera = new QToolButton(toolbar);
    camera->setObjectName("cameraMenuButton");
    camera->setIcon(icon(Glyph::Camera));
    camera->setToolTip("Camera views · F to frame character");
    camera->setPopupMode(QToolButton::InstantPopup);
    auto cameraMenu = new QMenu(camera);
    for (auto prefix : {"Frame character", "Perspective", "Front", "Side", "Top"})
        if (auto a = find(prefix))
            cameraMenu->addAction(a);
    camera->setMenu(cameraMenu);
    toolbar->addWidget(camera);
    auto selection = new QAction(icon(Glyph::Select), "Select", this);
    selection->setObjectName("selectAction");
    selection->setShortcut(QKeySequence("V"));
    selection->setCheckable(true);
    selection->setToolTip("Select controls (V)");
    connect(selection, &QAction::triggered, this, [this] {
        viewport->tool = Viewport::Select;
        refresh();
    });
    addAction(selection);
    auto modes = new QActionGroup(this);
    modes->setExclusive(true);
    for (auto a : {selection, moveAction, rotateAction})
        modes->addAction(a);
    moveAction->setChecked(true);
    connect(&workspace, &Workspace::changed, this, [this, selection] {
        QSignalBlocker block(selection);
        selection->setChecked(viewport->tool == Viewport::Select);
    });
    auto tools = new QToolBar("Viewport tools", this);
    tools->setObjectName("viewportTools");
    tools->setOrientation(Qt::Vertical);
    tools->setIconSize({21, 21});
    tools->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tools->setStyleSheet("QToolBar { border: 0; padding: 3px; spacing: 3px; } QToolButton { width: 28px; "
                         "height: 28px; border: 0; border-radius: 3px; } QToolButton:hover { background: "
                         "#41464d; } QToolButton:checked { background: #244d60; }");
    for (auto a : {selection, moveAction, rotateAction})
        tools->addAction(a);
    tools->addSeparator();
    if (auto a = find("Frame character"))
        tools->addAction(a);
    auto workspaceLayout = qobject_cast<QVBoxLayout *>(centralWidget()->layout());
    workspaceLayout->removeWidget(viewport);
    auto area = new QWidget;
    auto row = new QHBoxLayout(area);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(tools);
    row->addWidget(viewport, 1);
    workspaceLayout->addWidget(area, 1);
    auto outliner = findChild<QDockWidget *>("skeletonDock");
    auto content = new QWidget;
    auto layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto filterRow = new QHBoxLayout;
    auto search = new QLineEdit(content);
    search->setObjectName("outlinerSearch");
    search->setPlaceholderText("Filter objects");
    search->setClearButtonEnabled(true);
    filterRow->addWidget(search, 1);
    for (bool expand : {true, false}) {
        auto button = new QToolButton(content);
        button->setObjectName(expand ? "expandHierarchyButton" : "collapseHierarchyButton");
        button->setIcon(icon(expand ? Glyph::Expand : Glyph::Collapse));
        button->setToolTip(expand ? "Expand hierarchy" : "Collapse hierarchy");
        connect(button, &QToolButton::clicked, tree,
                expand ? &QTreeWidget::expandAll : &QTreeWidget::collapseAll);
        filterRow->addWidget(button);
    }
    layout->addLayout(filterRow);
    layout->addWidget(tree);
    outliner->setWidget(content);
    tree->setIndentation(16);
    tree->setAlternatingRowColors(false);
    tree->setUniformRowHeights(true);
    tree->setMinimumWidth(180);
    connect(search, &QLineEdit::textChanged, this, [this](QString value) {
        std::function<bool(QTreeWidgetItem *)> match = [&](QTreeWidgetItem *item) {
            bool visible = item->text(0).contains(value, Qt::CaseInsensitive);
            for (int i = 0; i < item->childCount(); ++i)
                visible = match(item->child(i)) || visible;
            item->setHidden(!visible);
            if (!value.isEmpty() && visible)
                item->setExpanded(true);
            return visible;
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            match(tree->topLevelItem(i));
    });
    // Keep the same transport object names so shortcuts and automation share the actions.
    auto transport = [&](QString name, Glyph glyph, QString tip) {
        auto button = findChild<QPushButton *>(name);
        if (!button)
            return;
        button->setText({});
        button->setIcon(icon(glyph));
        button->setIconSize({20, 20});
        button->setFixedSize(32, 28);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
    };
    transport("previousKeyButton", Glyph::Previous, "Previous key (,)");
    transport("nextKeyButton", Glyph::Next, "Next key (.)");
    transport("playButton", Glyph::Play, "Play / Pause (Space)");
    transport("insertKeyButton", Glyph::Key, "Insert key (I)");
    auto anim = findChild<QDockWidget *>("animationDock");
    for (auto button : anim->findChildren<QPushButton *>())
        if (button->objectName().isEmpty()) {
            bool prev = button->toolTip().startsWith("Previous");
            button->setText({});
            button->setIcon(icon(prev ? Glyph::Previous : Glyph::Next));
            button->setFixedSize(32, 28);
        }
    connect(&workspace, &Workspace::changed, this, [this] {
        findChild<QPushButton *>("playButton")
            ->setIcon(icon(document().playing ? Glyph::Pause : Glyph::Play));
    });
    for (auto button : selectionLabel->parentWidget()->findChildren<QPushButton *>()) {
        if (button->objectName() == "pinFeetButton") {
            button->setIcon(icon(Glyph::Feet));
            button->setText("Pin feet");
        } else if (button->text() == "Reset pose") {
            button->setIcon(icon(Glyph::Undo));
            button->setText("Rest pose");
        }
    }
    findChild<QDockWidget *>("inspectorDock")->setWindowTitle("Inspector");
    for (auto dock : findChildren<QDockWidget *>()) {
        dock->setMinimumWidth(dock == anim ? 200 : 270);
        dock->setStyleSheet(
            "QDockWidget::title { padding: 5px 7px; background: #30343a; } QGroupBox { font-weight: 600; "
            "border: 1px solid #3d4249; border-radius: 3px; margin-top: 10px; padding-top: 7px; } "
            "QGroupBox::title { subcontrol-origin: margin; left: 7px; padding: 0 3px; }");
    }
    // Add panels created after the View menu was built.
    for (auto menu : menuBar()->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly))
        if (menu->title() == "&View") {
            menu->addAction(findChild<QDockWidget *>("shapesDock")->toggleViewAction());
            menu->addAction(findChild<QDockWidget *>("physicsDock")->toggleViewAction());
        }
    QMap<QString, QAction *> menus;
    for (auto action : menuBar()->actions()) {
        menus[action->text()] = action;
        menuBar()->removeAction(action);
    }
    for (auto name : {"&File", "&Edit", "&Scene", "&Pose", "P&hysics", "&View", "&Help"})
        if (menus.contains(name))
            menuBar()->addAction(menus[name]);
    resetLayout();
}
} // namespace trickster
