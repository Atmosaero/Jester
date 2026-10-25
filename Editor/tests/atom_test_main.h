#pragma once
#ifndef Q_MOC_RUN
#include "settings/appearance.h"
#include "render/atom/AtomViewport.h"
#include <AtomToolsFramework/Application/AtomToolsApplication.h>
#include <AzCore/Debug/Trace.h>
#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Component/ComponentApplicationLifecycle.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <QTimer>
#include <QtTest>
#include <cstdio>

class JesterTestApplication final : public AtomToolsFramework::AtomToolsApplication
{
public:
    JesterTestApplication(const char* target,int* argc,char*** argv) : AtomToolsApplication(target,argc,argv) {}
    bool stopped=false;
    void Destroy() override {
        if(stopped) return;
        stopped=true;
        AtomToolsApplication::Destroy();
    }
    void StartCommon(AZ::Entity* entity) override {
        AtomToolsApplication::StartCommon(entity);
        trickster::applyAppearance(*this);
    }
private:
    void ConnectToAssetProcessor() override {
        auto* registry=AZ::SettingsRegistry::Get();
        AZStd::string catalog;
        registry->Get(catalog,AZ::SettingsRegistryMergeUtils::FilePathKey_CacheRootFolder);
        catalog+="/assetcatalog.xml";
        AZ::Data::AssetCatalogRequestBus::Broadcast(&AZ::Data::AssetCatalogRequests::LoadCatalog,catalog.c_str());
        AZ::ComponentApplicationLifecycle::SignalEvent(*registry,"CriticalAssetsCompiled",R"({})");
    }
};

template<class Test> int runJesterTests(int argc,char** argv,const char* target)
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    AZ::Debug::Trace trace;
    AzQtComponents::AzQtApplication::InitializeDpiScaling();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    JesterTestApplication app(target,&argc,&argv);
    app.setOrganizationName("JesterTests");
    app.setApplicationName(target);
    app.setQuitOnLastWindowClosed(false);
    QStringList arguments;
    for(const auto& argument:app.arguments())
        if(!argument.startsWith("--project-path=") && !argument.startsWith("--engine-path=") && !argument.startsWith("--rhi=")
            && !argument.startsWith("--regset=") && argument!="--unattended") arguments.push_back(argument);
    app.Start({},{});
    auto materialGuard=AtomViewport::createDefaultMaterial();
    QTimer engineTimer;
    QObject::connect(&engineTimer,&QTimer::timeout,&app,[&app] {
        app.TickSystem();app.Tick();
    });
    engineTimer.start(1);
    // qExec must run outside any Qt/engine callback: its nested modal loops
    // must not suspend the engine tick that dispatched the test suite.
    Test test;
    int result=QTest::qExec(&test,arguments);
    engineTimer.stop();
    materialGuard.reset();
    if(!app.stopped) app.Stop();
    return result;
}
#endif
