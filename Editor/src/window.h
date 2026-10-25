#pragma once
#include "physics_jobs.h"
#include "analysis_jobs.h"
#include "recovery.h"
#include "scene_catalog.h"
#include "timeline.h"
#include "viewport.h"
#include "workspace.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTreeWidget>
#include <thread>
namespace trickster {
class EditorApi;
class McpServer;
class Window : public QMainWindow {
    Q_OBJECT
  public:
    Window();
    ~Window() override;
    Workspace workspace;
    AnalysisJobs analysisJobs{workspace};
    double lastPanelRefreshMs=0;
    Document &document() const {
        return workspace.active();
    }
    Viewport *viewport;
    Timeline *timeline = nullptr;
    bool open(const QString &path);
    void saveDemo(const QString &directory);
    void chooseScene();
    void showGraphEditor();
    void showMotionAnalysis();
    void showRetarget();
    void newEmptyScene();
    void activateSceneChoice(const SceneChoice &choice);
    void switchScene(int index);
    void closeScene(int index);
    McpServer *mcpServer() const {
        return mcp;
    }

  protected:
    void closeEvent(QCloseEvent *) override;
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject *, QEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    McpServer *mcp = nullptr;
    void buildAutomation();
    void buildMotionPanel();
    void characterProfileDialog();
    void mirrorPairsDialog();
    void jointLimitDialog();
    void refreshMotionPanel();
    QSpinBox *correctionFrom = nullptr, *correctionTo = nullptr, *correctionFadeIn = nullptr,
             *correctionFadeOut = nullptr;
    QCheckBox *correctionEnabled = nullptr, *correctionEditing = nullptr;
    QDoubleSpinBox *correctionStrength = nullptr;
    QLabel *correctionStatus = nullptr;
    QComboBox *motionClip = nullptr;
    QSpinBox *motionBakeFps = nullptr;
    QTreeWidget *tree;
    void buildAdvancedPanels();
    void modernizeLayout();
    void resetLayout();
    void fitCompactLayout();
    bool compactLayout=false;
    QByteArray expandedLayout;
    void refreshPanels();
    QString fittedBodiesDocument;
    quint64 fittedBodiesRevision=~quint64(0);
    std::vector<RigidSegment> fittedBodiesCache;
    void previewPhysics(bool ballistic);

  public:
    bool startPhysicsTask(bool ballistic, int first, int last, QString &error);
    bool startPhysicsTask(const QString &operation,int first,int last,const QJsonObject &settings,QString &error);
    void showWholeBodyDialog();
    void physicsCalculationDialog(const QString &kind);
    void physicsModelDialog();
    void physicsProxyDialog();
    void physicsReportDialog();
    QJsonObject physicsTaskState() const;
    void cancelPhysicsTask();
    void pollPhysicsTask();
    bool hasPhysicsResult() const {
        return physicsJobs.hasResult(document());
    }
    std::optional<Scene> physicsResult() const {
        return physicsJobs.result(document());
    }
    void clearPhysicsResult();

  private:
    PhysicsJobs physicsJobs{workspace};
    RecoveryStore recovery{workspace};
    void buildRecovery();
    void recoverAutosave();
    QTimer physicsPoll;
    QProgressBar *physicsProgress = nullptr;
    QPushButton *cancelPhysics = nullptr;
    std::optional<Scene> physicsPreview;
    QTreeWidget *shapeTree = nullptr;
    std::vector<QDoubleSpinBox *> morphEditors;
    std::vector<QSlider *> morphSliders;
    std::vector<std::pair<int, int>> morphBindings;
    QDoubleSpinBox *gravityEdit = nullptr, *unitEdit = nullptr, *groundEdit = nullptr,
                   *frictionEdit = nullptr, *massEdit = nullptr, *radiusEdit = nullptr;
    std::array<QDoubleSpinBox *, 3> bodyCenter{}, bodyEnd{};
    QSpinBox *physicsFrom = nullptr, *physicsTo = nullptr;
    QLabel *physicsStats = nullptr;
    QPushButton *applyPhysics = nullptr;
    void rebuildShapes();
    QTabBar *sceneTabs;
    QWidget *emptyPanel;

    QMap<QString, Viewport::Camera> sceneCameras;
    QString cameraDocumentId;
    int nextSceneNumber = 1;
    bool switchingScene = false;
    void buildSceneWorkspace();

  public:
    void commitPendingEditor();
    void appendScene(Scene scene, QString path = {}, bool allowReuse = true);
    void refreshSceneTabs();
    void positionEmptyPanel();
    void renameScene();
    void duplicateScene();

  private:
    QLabel *selectionLabel;
    QLabel *mirrorLabel=nullptr,*recordingLabel=nullptr;
    QAction *symmetryAction=nullptr;
    QLabel *frameInfo;
    std::array<QDoubleSpinBox *, 3> position, rotation, target, pole;
    QCheckBox *pin;
    QDoubleSpinBox *weight;
    QCheckBox *limitEnabled;
    QDoubleSpinBox *limitAngle;
    QAction *assistAction;
    QWidget *ikBox;
    QSpinBox *current, *first, *last, *fps;
    QAction *playAction, *moveAction, *rotateAction, *localAction;
    QString motionPanelDocument;
    quint64 motionPanelRevision=~quint64(0);
    bool motionPanelOriginal=false;
    bool refreshing = false;
    QString lastDirectory;
    void buildHelpers();
    void contactDialog();
    void bakedCopyDialog(bool arc);
    void buildInspector();
    void buildTimeline();
    void buildActions();
    void rebuildTree();
    void refresh();
    void readTransform(bool rotate);
    void readIK(bool changePole);
    bool save(bool as = false);
    bool mayClose();
    void chooseOpen();
    void chooseExport();

  public:
    void report(QString text, bool error = false);
    void newCharacter(bool demo = false);
    void documentSaved() {
        recovery.discard(document().id);
        refresh();
    }
    bool applyOperation(const QString &name, const QJsonObject &arguments, QString label = {});
    void refreshView() {
        refresh();
    }
    QString outputDirectory() const {
        return lastDirectory;
    }
    void setOutputDirectory(QString path) {
        lastDirectory = path;
    }
};
} // namespace trickster
