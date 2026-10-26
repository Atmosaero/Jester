#include "ui/widgets/localized_ui.h"
#include "io/project/project_format.h"
#include "io/project/project_io.h"
#include "ui/window/window.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace trickster {
namespace {
class ScenePicker : public QDialog {
  public:
    SceneChoice chosen;
    bool openFile = false;
    explicit ScenePicker(QWidget *parent) : QDialog(parent) {
        setObjectName("scenePicker");
        bindUiText(this,"windowTitle","Jester · Choose scene");
        resize(1040, 680);
        setMinimumSize(820, 560);
        auto root = new QVBoxLayout(this);
        auto title = translatedWidget<QLabel>("Choose a scene", this);
        auto titleFont = title->font();
        titleFont.setPointSize(18);
        titleFont.setBold(true);
        title->setFont(titleFont);
        root->addWidget(title);
        auto hint = translatedWidget<QLabel>("Create an empty scene, start from a rig, or open a motion clip. Scenes stay "
                               "open in separate tabs.",
                               this);
        hint->setWordWrap(true);
        root->addWidget(hint);
        auto filters = new QHBoxLayout;
        search = new QLineEdit(this);
        search->setObjectName("sceneSearch");
        bindUiText(search,"placeholderText","Search scenes, characters and motions…");
        search->setClearButtonEnabled(true);
        category = new QComboBox(this);
        category->setObjectName("sceneCategory");
        addTranslatedItems(category,{"All scenes", "Templates", "Characters", "Motion clips", "Recent", "Local library"});
        filters->addWidget(search, 1);
        filters->addWidget(category);
        root->addLayout(filters);
        auto content = new QHBoxLayout;
        list = new QListWidget(this);
        list->setObjectName("sceneChoiceList");
        list->setMinimumWidth(300);
        list->setSpacing(3);
        content->addWidget(list, 4);
        auto right = new QVBoxLayout;
        preview = new Viewport(&previewDoc, this);
        preview->setObjectName("scenePreview");
        preview->showBones = false;
        preview->readOnly = true;
        bindToolTip(preview,"Drag left or right: orbit · Middle: pan · Wheel: zoom. Preview does not edit the source scene.");
        right->addWidget(preview, 1);
        details = new QLabel(this);
        details->setObjectName("sceneDetails");
        details->setWordWrap(true);
        details->setMinimumHeight(80);
        right->addWidget(details);
        previewPlay = translatedWidget<QPushButton>("Preview animation", this);
        previewPlay->setObjectName("previewAnimationButton");
        previewPlay->setEnabled(false);
        right->addWidget(previewPlay);
        content->addLayout(right, 6);
        root->addLayout(content, 1);
        auto bottom = new QHBoxLayout;
        auto folder = translatedWidget<QPushButton>("Add library folder…", this);
        folder->setObjectName("addLibraryFolderButton");
        bindUiText(folder,"toolTip","Choose a folder containing GLB, glTF or Jester projects.");
        auto file = translatedWidget<QPushButton>("Open file…", this);
        file->setObjectName("pickerOpenFileButton");
        bottom->addWidget(folder);
        bottom->addWidget(file);
        bottom->addStretch();
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
        openButton = buttons->button(QDialogButtonBox::Open);
        openButton->setObjectName("openSceneButton");
        bindUiText(openButton,"text","Open scene");
        openButton->setDefault(true);
        bottom->addWidget(buttons);
        root->addLayout(bottom);
        choices = sceneChoices();
        connect(search, &QLineEdit::textChanged, this, [this] { rebuild(); });
        connect(category, &QComboBox::currentTextChanged, this, [this] { rebuild(); });
        connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
            if (item)
                showChoice(item->data(Qt::UserRole).toInt());
        });
        connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
            if (openButton->isEnabled())
                accept();
        });
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(file, &QPushButton::clicked, this, [this] {
            openFile = true;
            accept();
        });
        connect(folder, &QPushButton::clicked, this, [this] {
            QString path = QFileDialog::getExistingDirectory(this, uiText("Add scene library"));
            if (path.isEmpty())
                return;
            QSettings().setValue("sceneLibraryFolder", path);
            choices = sceneChoices();
            rebuild();
        });
        connect(previewPlay, &QPushButton::clicked, this, [this] {
            previewDoc.play(!previewDoc.playing);
            previewPlay->setText(previewDoc.playing ? uiText("Pause preview") : uiText("Preview animation"));
        });
        rebuild();
        connect(&LocalizationService::instance(),&LocalizationService::languageChanged,this,[this] {
            const QSignalBlocker blocker(list);
            for(int row=0;row<list->count();++row) {
                auto *item=list->item(row);const auto &choice=choices[item->data(Qt::UserRole).toInt()];
                item->setText((choice.kind==SceneChoice::File?choice.name:uiText(choice.name))+uiText("\n")+uiText(choice.category));
            }
            refreshDetails();
        });
        search->setFocus();
    }

  private:
    Document previewDoc;
    Viewport *preview;
    QListWidget *list;
    QLineEdit *search;
    QComboBox *category;
    QLabel *details;
    QPushButton *previewPlay;
    QPushButton *openButton;
    std::vector<SceneChoice> choices;
    void rebuild() {
        QSignalBlocker blocker(list);
        list->clear();
        QString query = search->text().trimmed(), filter = choiceText(category);
        for (int i = 0; i < int(choices.size()); ++i) {
            const auto &choice = choices[i];
            if (filter != "All scenes" && choice.category != filter)
                continue;
            if (!query.isEmpty() && !(choice.name + " " + choice.description + " " + choice.category)
                                         .contains(query, Qt::CaseInsensitive))
                continue;
            auto item = new QListWidgetItem((choice.kind==SceneChoice::File?choice.name:uiText(choice.name)) + "\n" + uiText(choice.category), list);
            item->setData(Qt::UserRole, i);
            item->setToolTip(choice.description + (choice.path.isEmpty() ? QString() : uiText("\n") + choice.path));
            item->setIcon(style()->standardIcon(choice.kind == SceneChoice::Empty   ? QStyle::SP_DirIcon
                                                : choice.category == "Motion clips" ? QStyle::SP_MediaPlay
                                                                                    : QStyle::SP_FileIcon));
            item->setSizeHint(QSize(280, 54));
        }
        if (list->count()) {
            list->setCurrentRow(0);
            showChoice(list->item(0)->data(Qt::UserRole).toInt());
        } else {
            previewDoc.replace(Scene{});
            bindUiText(details,"text","No matching scenes. Try another name or open a file.");
            previewPlay->setEnabled(false);
            openButton->setEnabled(false);
        }
    }
    void showChoice(int i) {
        if (i < 0 || i >= int(choices.size()))
            return;
        chosen = choices[i];
        Scene scene;
        QString error;
        bool ok = true;
        if (chosen.kind == SceneChoice::Mannequin)
            scene = Scene::mannequin();
        else if (chosen.kind == SceneChoice::Demo) {
            Document temporary;
            temporary.demo();
            scene = std::move(temporary.scene);
        } else if (chosen.kind == SceneChoice::File) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            ok = isNativeProjectFile(chosen.path)
                     ? loadProject(chosen.path, scene, error)
                     : loadCharacter(chosen.path, scene, error);
            QApplication::restoreOverrideCursor();
        }
        if (!ok) {
            previewDoc.replace(Scene{});
            details->setText(uiText("Cannot preview this scene: ") + translatedDiagnostic(error));
            openButton->setEnabled(false);
            previewPlay->setEnabled(false);
            return;
        }
        previewDoc.replace(std::move(scene));
        previewDoc.select(-1);
        preview->fit();
        refreshDetails();
        openButton->setEnabled(true);
        previewPlay->setEnabled(!previewDoc.scene.keys.isEmpty());
        bindUiText(previewPlay,"text","Preview animation");
    }
    void refreshDetails() {
        QString text = chosen.kind==SceneChoice::File?chosen.description:uiText(chosen.description);
        if (!previewDoc.scene.nodes.empty())
            text += uiText("\n%1 nodes · %2 pose keys · %3 frames · %4 fps")
                        .arg(previewDoc.scene.nodes.size())
                        .arg(previewDoc.scene.keys.size())
                        .arg(previewDoc.scene.last - previewDoc.scene.first + 1)
                        .arg(previewDoc.scene.fps);
        if (!previewDoc.scene.warnings.isEmpty())
            text += "\n" + previewDoc.scene.warnings.join("\n");
        details->setText(text);
    }
};
} // namespace
void Window::commitPendingEditor() {
    QWidget *focused = QApplication::focusWidget();
    if (!focused || !isAncestorOf(focused))
        return;
    if (auto line = qobject_cast<QLineEdit *>(focused); line && line->objectName() == "motionLayerName") {
        line->clearFocus();
        return;
    }
    for (QWidget *editorWidget = focused; editorWidget && editorWidget != this;
         editorWidget = editorWidget->parentWidget())
        if (auto spin = qobject_cast<QAbstractSpinBox *>(editorWidget)) {
            spin->interpretText();
            focused->clearFocus();
            return;
        }
}
void Window::buildSceneWorkspace() {
    sceneCameras[document().id]=viewport->camera();
    sceneTabs->addTab(document().scene.name);
    sceneTabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sceneTabs, &QTabBar::currentChanged, this, &Window::switchScene);
    connect(sceneTabs, &QTabBar::tabCloseRequested, this, &Window::closeScene);
    connect(sceneTabs, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index >= 0) {
            switchScene(index);
            renameScene();
        }
    });
    connect(sceneTabs, &QTabBar::customContextMenuRequested, this, [this](QPoint pos) {
        int index = sceneTabs->tabAt(pos);
        if (index < 0)
            return;
        switchScene(index);
        QMenu menu;
        auto rename = addTranslatedAction(&menu,"Rename scene…"), duplicate = addTranslatedAction(&menu,"Duplicate scene"),
             close = addTranslatedAction(&menu,"Close scene");
        auto chosen = menu.exec(sceneTabs->mapToGlobal(pos));
        if (chosen == rename)
            renameScene();
        else if (chosen == duplicate)
            duplicateScene();
        else if (chosen == close)
            closeScene(index);
    });
    auto menu = addTranslatedMenu(menuBar(),"&Scene");
    menu->setObjectName("sceneMenu");
    auto choose = addTranslatedAction(menu,"Choose scene…");
    choose->setObjectName("chooseSceneAction");
    choose->setShortcut(QKeySequence("Ctrl+Shift+O"));
    connect(choose, &QAction::triggered, this, &Window::chooseScene);
    auto newScene = addTranslatedAction(menu,"New empty scene");
    connect(newScene, &QAction::triggered, this, &Window::newEmptyScene);
    auto duplicate = addTranslatedAction(menu,"Duplicate current scene");
    connect(duplicate, &QAction::triggered, this, &Window::duplicateScene);
    auto rename = addTranslatedAction(menu,"Rename current scene…");
    connect(rename, &QAction::triggered, this, &Window::renameScene);
    auto close = addTranslatedAction(menu,"Close scene");
    close->setShortcut(QKeySequence("Ctrl+W"));
    connect(close, &QAction::triggered, this, [this] { closeScene(workspace.activeIndex); });
    auto info = addTranslatedAction(menu,"Scene information…");
    connect(info, &QAction::triggered, this, [this] {
        QString text = uiText("%1\n%2 nodes · %3 meshes · %4 keys\n\n%5")
                           .arg(document().scene.name)
                           .arg(document().scene.nodes.size())
                           .arg(document().scene.meshes.size())
                           .arg(document().scene.keys.size())
                           .arg(document().scene.warnings.isEmpty() ? uiText("No import notes.")
                                                                    : document().scene.warnings.join("\n"));
        showUiMessage(QMessageBox::Information,this,[&] {return uiText("Jester · Scene information");},[&] {return translatedDiagnostic(text);});
    });
    auto toolbar = findChild<QToolBar *>("mainToolbar");
    toolbar->insertAction(findChild<QAction *>("openAction"), choose);
    emptyPanel = new QWidget(viewport);
    emptyPanel->setObjectName("emptyScenePanel");
    auto panelLayout = new QVBoxLayout(emptyPanel);
    panelLayout->setContentsMargins(20, 20, 20, 20);
    auto label = translatedWidget<QLabel>("Empty scene", emptyPanel);
    auto font = label->font();
    font.setPointSize(20);
    font.setBold(true);
    label->setFont(font);
    label->setAlignment(Qt::AlignCenter);
    panelLayout->addWidget(label);
    auto hint = translatedWidget<QLabel>("Choose a character, animation or template to start.\nEach scene has its own "
                           "poses, timeline and undo history.",
                           emptyPanel);
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    panelLayout->addWidget(hint);
    auto chooseButton = translatedWidget<QPushButton>("Choose scene…    Ctrl+Shift+O", emptyPanel);
    chooseButton->setObjectName("emptyChooseSceneButton");
    connect(chooseButton, &QPushButton::clicked, this, &Window::chooseScene);
    panelLayout->addWidget(chooseButton);
    auto row = new QHBoxLayout;
    auto open = translatedWidget<QPushButton>("Open file…", emptyPanel);
    open->setObjectName("emptyOpenFileButton");
    auto mannequin = translatedWidget<QPushButton>("New mannequin", emptyPanel);
    mannequin->setObjectName("emptyMannequinButton");
    connect(open, &QPushButton::clicked, this, &Window::chooseOpen);
    connect(mannequin, &QPushButton::clicked, this, [this] { newCharacter(); });
    row->addWidget(open);
    row->addWidget(mannequin);
    panelLayout->addLayout(row);
    emptyPanel->setAutoFillBackground(true);
    viewport->installEventFilter(this);
    positionEmptyPanel();
}
void Window::positionEmptyPanel() {
    if (!emptyPanel)
        return;
    int w = std::min(440, std::max(280, viewport->width() - 32));
    emptyPanel->setGeometry((viewport->width() - w) / 2, std::max(40, (viewport->height() - 240) / 2), w,
                            240);
    emptyPanel->raise();
}
bool Window::eventFilter(QObject *object, QEvent *event) {
    if (object == viewport && event->type() == QEvent::Resize)
        positionEmptyPanel();
    return QMainWindow::eventFilter(object, event);
}
void Window::refreshSceneTabs() {
    QSignalBlocker blocker(sceneTabs);
    while (sceneTabs->count() > int(workspace.documents.size()))
        sceneTabs->removeTab(sceneTabs->count() - 1);
    while (sceneTabs->count() < int(workspace.documents.size()))
        sceneTabs->addTab("");
    for (int i = 0; i < int(workspace.documents.size()); ++i) {
        const auto &d = *workspace.documents[i];
        sceneTabs->setTabText(i, d.scene.name + (d.dirty() ? " *" : ""));
        sceneTabs->setTabToolTip(i, d.path);
    }
    sceneTabs->setCurrentIndex(workspace.activeIndex);
}
void Window::appendScene(Scene scene, QString path, bool allowReuse) {
    commitPendingEditor();
    sceneCameras[document().id] = viewport->camera();
    switchingScene = true;
    workspace.append(std::move(scene), path, allowReuse);
    sceneCameras[document().id] = viewport->camera();
    switchingScene = false;
    refreshSceneTabs();
}
void Window::switchScene(int index) {
    if (switchingScene || index < 0 || index >= int(workspace.documents.size()) ||
        index == workspace.activeIndex)
        return;
    commitPendingEditor();
    sceneCameras[document().id] = viewport->camera();
    switchingScene = true;
    workspace.activate(index);
    viewport->setCamera(sceneCameras.value(document().id));
    switchingScene = false;
    refreshSceneTabs();
}
void Window::closeScene(int index) {
    commitPendingEditor();
    if (index < 0 || index >= int(workspace.documents.size()))
        return;
    int previous = workspace.activeIndex;
    switchScene(index);
    if (!mayClose()) {
        switchScene(previous);
        return;
    }
    switchingScene = true;
    const auto removedId=document().id;
    recovery.discard(removedId);
    workspace.remove(index);
    sceneCameras.remove(removedId);
    viewport->setCamera(sceneCameras[document().id]);
    switchingScene = false;
    refreshSceneTabs();
}
void Window::buildRecovery() {
    auto fileMenu = menuBar()->actions().front()->menu();
    auto recover = addTranslatedAction(fileMenu,"Recover autosave…");
    recover->setObjectName("recoverAutosaveAction");
    connect(recover, &QAction::triggered, this, &Window::recoverAutosave);
    connect(&recovery, &RecoveryStore::failed, this, [this](QString error) { report(error); });
    if (QCoreApplication::applicationName() == "Jester") {
        recovery.start();
        if (!recovery.snapshots().empty()) {
            auto recoverButton = translatedWidget<QPushButton>("Recover autosave", this);
            recoverButton->setObjectName("recoverAutosaveBanner");
            statusBar()->addPermanentWidget(recoverButton);
            connect(recoverButton, &QPushButton::clicked, this, &Window::recoverAutosave);
            report(QString("Recovery snapshots available: File → Recover autosave. Original files are untouched."));
        }
    }
}
void Window::recoverAutosave() {
    commitPendingEditor();
    auto path = QFileDialog::getOpenFileName(this, uiText("Recover autosave"), recovery.directory(),
                                             uiText("Jester recovery projects (*.recovery.jst)"));
    if (path.isEmpty())
        return;
    QString error;
    sceneCameras[document().id] = viewport->camera();
    if (!recovery.recover(path, error)) {
        report(error);
        return;
    }
    viewport->fit();
    refreshSceneTabs();
    report(QString("Recovered as an unsaved document. Save As creates a new project."));
}
void Window::newEmptyScene() {
    Scene empty;
    empty.name = uiText("Untitled scene %1").arg(++nextSceneNumber);
    appendScene(std::move(empty), {}, false);
    report(QString("New empty scene. Choose a scene or open a character to begin."));
}
void Window::activateSceneChoice(const SceneChoice &choice) {
    if (choice.kind == SceneChoice::File) {
        open(choice.path);
        return;
    }
    if (choice.kind == SceneChoice::Empty) {
        if (!document().scene.nodes.empty() || document().dirty() || !document().path.isEmpty())
            newEmptyScene();
        else
            report(QString("Empty scene ready. Open a character when needed."));
    } else
        newCharacter(choice.kind == SceneChoice::Demo);
}
void Window::chooseScene() {
    commitPendingEditor();
    document().play(false);
    ScenePicker dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (dialog.openFile)
        chooseOpen();
    else
        activateSceneChoice(dialog.chosen);
}
void Window::renameScene() {
    bool ok = false;
    QString name = QInputDialog::getText(this,uiText("Rename scene"),uiText("Scene name"), QLineEdit::Normal,
                                         document().scene.name, &ok)
                       .trimmed();
    if (ok && !name.isEmpty() && name != document().scene.name)
        document().edit("Rename scene", [&](Scene &scene) { scene.name = name; }, false);
}
void Window::duplicateScene() {
    commitPendingEditor();
    Scene copy = document().scene;
    copy.name += " copy";
    appendScene(std::move(copy), {}, false);
    document().undo.resetClean();
    refresh();
    report(QString("Scene duplicated. Save the copy as a new project."));
}
} // namespace trickster
