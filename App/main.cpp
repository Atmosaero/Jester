#include <AtomToolsFramework/Application/AtomToolsApplication.h>
#include <AtomToolsFramework/Viewport/RenderViewportWidget.h>
#include <Atom/Feature/Utils/FrameCaptureBus.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <AzCore/Debug/Trace.h>
#include <AzCore/Debug/TraceMessageBus.h>
#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Component/ComponentApplicationLifecycle.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <QMainWindow>
#include <QTimer>
#include <QScreen>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QImage>
#include "render/atom/AtomViewport.h"
#include "ui/window/window.h"
#include "io/project/project_format.h"
#include "settings/appearance.h"
#include "ui/widgets/icons.h"
#include "ui/widgets/localized_ui.h"
#include "trickster_version.h"
#include "automation/mcp_server.h"
#include <QSaveFile>
#include <QFileInfo>
#include <QSettings>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <clocale>

namespace {
QStringList sceneFileArguments(const QStringList &arguments) {
    QStringList files;
    bool literal=false;
    for(int index=1;index<arguments.size();++index) {
        const auto &argument=arguments[index];
        if(!literal) {
            if(argument=="--") {literal=true;continue;}
            if(argument=="--mcp-port" || argument=="--mcp-config" || argument=="--screenshot" || argument=="--generate-demo" ||
               argument=="--project-path" || argument=="--engine-path" || argument=="--rhi" || argument=="--regset") {++index;continue;}
            if(argument.startsWith('-'))continue;
        }
        files.append(argument);
    }
    return files;
}
}
class JesterTrace final : public AZ::Debug::TraceMessageBus::Handler
{
public:
    JesterTrace() { BusConnect(); }
    ~JesterTrace() { BusDisconnect(); }
    bool OnError(const char*, const char*) override { ++errors; return false; }
    bool OnAssert(const char*) override { ++asserts; return false; }
    std::atomic<int> errors{0}, asserts{0};
};

class JesterApplication final : public AtomToolsFramework::AtomToolsApplication,
    private AZ::Render::FrameCaptureNotificationBus::Handler
{
public:
    JesterApplication(int* argc, char*** argv, JesterTrace* validation)
        : AtomToolsApplication("JesterEditor", argc, argv), m_validation(validation) {}
    int result() const { return m_result; }
    bool stopped() const { return m_stopped; }

    void RunMainLoop() override
    {
        // Qt owns dispatch. Do not pump Qt from inside an engine tick: MCP
        // captures and modal tools use nested loops and must remain responsive.
        QTimer engineTimer;
        connect(&engineTimer,&QTimer::timeout,this,[this] {
            if(WasExitMainLoopRequested()) { quit();return; }
            TickSystem();
            Tick();
        });
        engineTimer.start(1);
        QTimer::singleShot(0,this,[this] { ProcessCommandLine(m_commandLine); });
        exec();
    }

    void StartCommon(AZ::Entity* systemEntity) override
    {
        AtomToolsApplication::StartCommon(systemEntity);
        // SDK 26.10 requires at least one registered material type when its
        // SceneMaterialSrg is first compiled, including an empty editor scene.
        m_materialGuard=AtomViewport::createDefaultMaterial();
        if(!m_materialGuard) {
            qCritical("Jester materials are not prepared. Run build.ps1 -Assets first.");
            m_result=2;
            QTimer::singleShot(0,this,&QApplication::quit);
            return;
        }
        setApplicationName("Jester");
        setOrganizationName("Jester");
        setApplicationVersion(TRICKSTER_VERSION);
        setWindowIcon(trickster::editorLogo());
        if(!arguments().contains("--poc") && !arguments().contains("--smoke"))
        {
            setQuitOnLastWindowClosed(true);
            trickster::applyAppearance(*this);
            auto* editor=new trickster::Window;
            m_window=editor;
            if(arguments().contains("--demo")) editor->document().demo();
            for(const auto &argument:sceneFileArguments(arguments())) {
                if(!editor->open(argument)) {
                    qCritical("Cannot open scene: use a Jester project (.jst), FBX, GLB or glTF file.");
                    m_result=2;QTimer::singleShot(0,this,&QApplication::quit);return;
                }
            }
            if(!option("--generate-demo").isEmpty()) editor->saveDemo(option("--generate-demo"));
            if(arguments().contains("--mcp"))
            {
                bool valid=false;
                auto raw=option("--mcp-port");
                uint port=(raw.isEmpty()?QSettings().value("automation/port",18530).toString():raw).toUInt(&valid);
                if(!valid || port>65535 || !editor->mcpServer()->start(quint16(port)))
                {
                    qCritical("Cannot start Jester MCP server");m_result=2;
                    QTimer::singleShot(0,this,&QApplication::quit);return;
                }
                auto configPath=option("--mcp-config");
                if(!configPath.isEmpty())
                {
                    QSaveFile config(configPath);
                    if(!config.open(QIODevice::WriteOnly) || config.write(QJsonDocument(editor->mcpServer()->connectionConfig()).toJson())<0 || !config.commit())
                    { qCritical("Cannot save MCP connection config");m_result=2;QTimer::singleShot(0,this,&QApplication::quit);return; }
                }
            }
            editor->show();
            auto *frame=trickster::editorWindowFrame(editor);
            QSettings windowSettings;
            if(windowSettings.contains("blueJayGeometry"))frame->restoreGeometry(windowSettings.value("blueJayGeometry").toByteArray());
            const auto available=frame->screen()->availableGeometry();
            if(!available.contains(frame->frameGeometry()))
            {
                auto extra=frame->frameGeometry().size()-frame->size();
                frame->resize(std::min(frame->width(),available.width()-std::max(0,extra.width())),
                    std::min(frame->height(),available.height()-std::max(0,extra.height())));
                frame->move(available.topLeft());
            }
            auto screenshot=option("--screenshot");
            if(!screenshot.isEmpty()) QTimer::singleShot(2500,this,[this,editor,screenshot] {
                QDir().mkpath(QFileInfo(screenshot).absolutePath());
                auto image=trickster::captureEditorWidget(editor);
                bool saved=!image.isNull() && image.save(screenshot);
                trickster::captureEditorWidget(editor->viewport).save(QFileInfo(screenshot).absolutePath()+"/viewport.png");
                editor->document().undo.setClean();m_result=saved?0:2;quit();
            });
            return;
        }
        m_window = new QMainWindow;
        m_window->setWindowTitle("Jester 3D — Atom viewport");
        m_window->resize(1280, 800);
        m_viewport = new AtomViewport(m_window);
        m_window->setCentralWidget(m_viewport);
        m_window->show();
        if (arguments().contains("--smoke"))
        {
            m_result = 2;
            QTimer::singleShot(2000, this, [this] { m_before = static_cast<AtomViewport*>(m_viewport)->diagnostics(); m_window->resize(1024,700); });
            QTimer::singleShot(3500, this, [this] { static_cast<AtomViewport*>(m_viewport)->exerciseCamera(); });
            QTimer::singleShot(5000, this, [this] { m_window->showMinimized(); });
            QTimer::singleShot(6500, this, [this] { m_window->showNormal(); m_window->resize(1360,850); });
            QTimer::singleShot(12000, this, [this] {
                QDir().mkpath("artifacts");
                AZ::Render::FrameCaptureOutcome capture;
                const AZStd::string path(QDir().absoluteFilePath("artifacts/poc-frame.ppm").toUtf8().constData());
                AZ::Render::FrameCaptureRequestBus::BroadcastResult(capture,
                    &AZ::Render::FrameCaptureRequests::CaptureScreenshotForWindow,
                    path, m_viewport->GetViewportContext()->GetWindowHandle());
                if (capture.IsSuccess()) AZ::Render::FrameCaptureNotificationBus::Handler::BusConnect(capture.GetValue());
                else finishSmoke(false);
            });
            QTimer::singleShot(30000,this,[this] { finishSmoke(false); });
        }
    }

    void Destroy() override
    {
        if(m_stopped) return;
        m_stopped=true;
        AZ::Render::FrameCaptureNotificationBus::Handler::BusDisconnect();
        delete m_window;
        m_window = nullptr;
        m_viewport = nullptr;
        m_materialGuard.reset();
        AtomToolsApplication::Destroy();
    }

private:
    QString option(const QString& name) const
    {
        const auto values=arguments();
        for(int i=1;i<values.size();++i) {
            if(values[i].startsWith(name+"=")) return values[i].mid(name.size()+1);
            if(values[i]==name && i+1<values.size()) return values[i+1];
        }
        return {};
    }
    void OnFrameCaptureFinished(AZ::Render::FrameCaptureResult result, const AZStd::string&) override
    {
        QTimer::singleShot(0,this,[this,result] { finishSmoke(result==AZ::Render::FrameCaptureResult::Success); });
    }
    void finishSmoke(bool captured)
    {
        if (m_smokeFinished) return;
        m_smokeFinished=true;
        AZ::Render::FrameCaptureNotificationBus::Handler::BusDisconnect();
        auto after=static_cast<AtomViewport*>(m_viewport)->diagnostics();
        QImage image("artifacts/poc-frame.ppm");
        int colored=0;
        if (!image.isNull()) for (int y=0;y<image.height();y+=8) for(int x=0;x<image.width();x+=8)
        {
            auto pixel=image.pixelColor(x,y);
            if (std::max({pixel.red(),pixel.green(),pixel.blue()})-std::min({pixel.red(),pixel.green(),pixel.blue()})>20) ++colored;
        }
        bool screenshot=captured && !image.isNull() && image.save("artifacts/poc-viewport.png");
        bool ok=screenshot && colored>50 && after["loadedModels"].toInt()==4 && after["renderTicks"].toInteger()>2
            && after["cameraYaw"]!=m_before["cameraYaw"] && after["cameraDistance"]!=m_before["cameraDistance"]
            && m_validation->errors.load()==0 && m_validation->asserts.load()==0;
        QJsonObject report{{"before",m_before},{"after",after},{"screenshot",screenshot},{"coloredSamples",colored},{"passed",ok},
            {"errors",m_validation->errors.load()},{"asserts",m_validation->asserts.load()}};
        QFile file("artifacts/poc-smoke.json");
        if(file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(report).toJson());
        m_result=ok?0:2;
        quit();
    }
    void ConnectToAssetProcessor() override
    {
        if (arguments().contains("--process-assets"))
        {
            AtomToolsApplication::ConnectToAssetProcessor();
            return;
        }
        // A prepared project runs independently from Asset Processor.
        auto* registry = AZ::SettingsRegistry::Get();
        AZStd::string catalog;
        registry->Get(catalog, AZ::SettingsRegistryMergeUtils::FilePathKey_CacheRootFolder);
        catalog += "/assetcatalog.xml";
        AZ::Data::AssetCatalogRequestBus::Broadcast(&AZ::Data::AssetCatalogRequests::LoadCatalog, catalog.c_str());
        AZ::ComponentApplicationLifecycle::SignalEvent(*registry, "CriticalAssetsCompiled", R"({})");
    }
    AZStd::vector<AZStd::string> GetCriticalAssetFilters() const override
    {
        return {"passes/", "config/", "materials/jesterdefault"};
    }
    QMainWindow* m_window = nullptr;
    AZ::Data::Instance<AZ::RPI::Material> m_materialGuard;
    AtomToolsFramework::RenderViewportWidget* m_viewport = nullptr;
    int m_result = 0;
    JesterTrace* m_validation;
    QJsonObject m_before;
    bool m_smokeFinished=false;
    bool m_stopped=false;
};

int main(int argc, char** argv)
{
#ifdef _WIN32
    // The SDK's Windows module loader uses mbstowcs_s for UTF-8 paths.
    // Configure character conversion only; numeric serialization stays in C locale.
    if (!std::setlocale(LC_CTYPE, ".UTF8")) {
        std::fputs("Jester requires Windows UTF-8 locale support.\n", stderr);
        return 2;
    }
#endif
    bool mcp=false,mcpSettings=false;
    for(int i=1;i<argc;++i) {
        if(!std::strcmp(argv[i],"--help") || !std::strcmp(argv[i],"-h")) {
            std::printf("Jester %s - Atom character animation editor\n"
                "Usage: Jester.exe [options] [file.fbx|file.glb|file.gltf|file.jst]\n"
                "  --project-path <folder>    Optional SDK project root; portable builds auto-detect it\n"
                "  --demo                    Open the built-in animated rig\n"
                "  --generate-demo <folder>  Write demo project and GLB\n"
                "  --screenshot <path>       Capture editor and viewport, then exit\n"
                "  --mcp                     Start native local MCP\n"
                "  --mcp-port <port>         Port (default 18530; 0 selects a free port)\n"
                "  --mcp-config <path>       Write local connection JSON\n"
                "  --poc / --smoke           Open / validate the Atom viewport proof\n"
                "  --build-info / --version  Print product/build identity\n",TRICKSTER_VERSION);
            return 0;
        }
        if(!std::strcmp(argv[i],"--build-info")) { std::printf("Jester %s (%s)\n",TRICKSTER_VERSION,TRICKSTER_BUILD_ID);return 0; }
        if(!std::strcmp(argv[i],"--version")) { std::printf("Jester %s\n",TRICKSTER_VERSION);return 0; }
        if(!std::strcmp(argv[i],"--mcp")) mcp=true;
        if(!std::strcmp(argv[i],"--mcp-port") || !std::strcmp(argv[i],"--mcp-config")
            || !std::strncmp(argv[i],"--mcp-port=",11) || !std::strncmp(argv[i],"--mcp-config=",13)) mcpSettings=true;
    }
    if(mcpSettings && !mcp) { std::fprintf(stderr,"--mcp-port/--mcp-config require --mcp\n");return 2; }
    QStringList startupArguments;
    for(int i=0;i<argc;++i)startupArguments.append(QString::fromUtf8(argv[i]));
    for(const auto &file:sceneFileArguments(startupArguments)) {
        if(!trickster::isSupportedSceneFile(file)) {
            std::fputs("Jester: unsupported scene file. Use .jst, .fbx, .glb or .gltf.\n",stderr);
            return 2;
        }
        if(!QFileInfo(file).isFile()) {
            std::fprintf(stderr,"Jester: scene file not found: %s\n",file.toUtf8().constData());
            return 2;
        }
    }
    AZ::Debug::Trace trace;
    JesterTrace validation;
    AzQtComponents::AzQtApplication::InitializeDpiScaling();
    JesterApplication app(&argc, &argv, &validation);
#ifdef _WIN32
    // Restore this after Qt/application construction, before SDK modules load.
    std::setlocale(LC_CTYPE, ".UTF8");
#endif
    app.Start({}, {});
    app.RunMainLoop();
    if(!app.stopped()) app.Stop();
    return app.result() ? app.result() : (validation.errors.load() || validation.asserts.load() ? 2 : 0);
}
