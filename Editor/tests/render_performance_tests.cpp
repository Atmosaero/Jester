#include "ui/window/window.h"
#ifndef Q_MOC_RUN
#include "render/atom/AtomViewport.h"
#endif
#include "render_fixtures.h"
#include "atom_test_main.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QtTest>
#include <algorithm>
#include <numeric>
#if defined(_WIN32) && !defined(Q_MOC_RUN)
#include <Windows.h>
#include <Psapi.h>
#endif
using namespace trickster;
namespace {
double processCpuMs() {
#ifdef _WIN32
    FILETIME created,ended,kernel,user;
    if(GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)) {
        ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;
        return double(k.QuadPart+u.QuadPart)/10000.;
    }
#endif
    return -1;
}
QJsonObject memory() {
    QJsonObject result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters)))
        result={{"workingSetMiB",double(counters.WorkingSetSize)/1048576.},
                {"privateCommitMiB",double(counters.PrivateUsage)/1048576.}};
#endif
    return result;
}
QJsonObject distribution(std::vector<double> values) {
    if(values.empty())return {};
    std::sort(values.begin(),values.end());
    auto percentile=[&](double p){return values[std::min(values.size()-1,size_t(std::ceil(p*values.size())-1))];};
    return {{"samples",int(values.size())},{"mean",std::accumulate(values.begin(),values.end(),0.)/values.size()},
            {"p50",percentile(.5)},{"p95",percentile(.95)},{"max",values.back()}};
}
}
class RenderPerformanceTests : public QObject {
    Q_OBJECT
  private slots:
    void benchmark_data() {
        QTest::addColumn<QString>("kind");
        for(auto name:{"simple","heavy-morph","multi-mesh"})QTest::newRow(name)<<QString(name);
    }
    void benchmark() {
        QFETCH(QString,kind);
        Window window;window.resize(1280,720);window.show();
        auto fixture=render_fixture::scene(kind);
        QString error;QVERIFY2(fixture.validate(&error),qPrintable(error));
        size_t vertices=0;for(const auto &mesh:fixture.meshes)vertices+=mesh.vertices.size();
        window.document().replace(fixture);window.viewport->showBones=false;
        window.viewport->showBalance=false;window.viewport->showTrajectory=false;
        window.viewport->forceCpuSkinning=qEnvironmentVariableIsSet("JESTER_BENCHMARK_CPU");
        window.viewport->fit();
        QTRY_VERIFY_WITH_TIMEOUT(window.viewport->renderingStats()["drawPackets"].toInt()>0,15000);
        QTest::qWait(2000);
        AtomViewport *renderer=nullptr;
        for(auto *object:window.viewport->findChildren<QObject*>())
            if(auto *candidate=dynamic_cast<AtomViewport*>(object)) {renderer=candidate;break;}
        QVERIFY(renderer);
        auto original=renderer->onFrame;
        QElapsedTimer clock;clock.start();
        std::vector<double> callbacks;
        struct RestoreCallback {
            AtomViewport *renderer;
            std::function<void()> callback;
            ~RestoreCallback() {renderer->onFrame=callback;}
        } restore{renderer,original};
        renderer->onFrame=[&] {callbacks.push_back(clock.nsecsElapsed()/1e6);if(original)original();};
        const auto initial=window.viewport->renderingStats();
        const auto initialMemory=memory();
        const auto cpuStart=processCpuMs();
        std::vector<double> seekCpu,seekCallback;
        for(int i=0;i<30;++i) {
            const auto before=window.viewport->renderingStats()["skinUpdates"].toInteger();
            QElapsedTimer latency;latency.start();
            window.document().seek((i*37+13)%91);
            seekCpu.push_back(latency.nsecsElapsed()/1e6);
            while(window.viewport->renderingStats()["skinUpdates"].toInteger()<=before && latency.elapsed()<5000)QTest::qWait(1);
            QVERIFY(window.viewport->renderingStats()["skinUpdates"].toInteger()>before);
            const auto callbackAfterUpload=callbacks.size();
            while(callbacks.size()<=callbackAfterUpload && latency.elapsed()<5000)QTest::qWait(1);
            QVERIFY(callbacks.size()>callbackAfterUpload);
            seekCallback.push_back(latency.nsecsElapsed()/1e6);
        }
        callbacks.clear();clock.restart();
        window.document().play(true);QTest::qWait(3000);window.document().play(false);
        const auto measured=window.viewport->renderingStats();
        const auto cpuMs=processCpuMs()-cpuStart;
        const auto playbackMemory=memory();
        std::vector<double> intervals;
        for(size_t i=1;i<callbacks.size();++i)intervals.push_back(callbacks[i]-callbacks[i-1]);
        renderer->onFrame=original;
        const auto output=qEnvironmentVariable("JESTER_BENCHMARK_OUTPUT","artifacts/render-performance");
        QVERIFY(QDir().mkpath(output));
        std::vector<double> captureTimes;
        for(int i=0;i<3;++i) {
            QElapsedTimer timer;timer.start();auto image=window.viewport->grabFramebuffer();
            captureTimes.push_back(timer.nsecsElapsed()/1e6);QVERIFY(!image.isNull());
            if(i==0)QVERIFY(image.save(output+"/"+kind+".png"));
        }
        QJsonObject report{{"case",kind},{"vertices",qint64(vertices)},{"meshes",int(fixture.meshes.size())},
            {"morphTargets",kind=="heavy-morph"?2:0},{"windowWidth",1280},{"windowHeight",720},
            {"scope","CPU and RPI preparation callbacks; no GPU timer or physical presentation measurement"},
            {"seekSynchronousCpuMs",distribution(seekCpu)},{"seekToPreparationCallbackMs",distribution(seekCallback)},
            {"preparationCallbackIntervalMs",distribution(intervals)},
            {"framebufferCaptureMs",distribution(captureTimes)},{"processCpuMsExcludingCaptures",cpuMs},
            {"memoryAfterWarmup",initialMemory},{"memoryAfterPlayback",playbackMemory},{"memoryAfterCaptures",memory()},
            {"rendererBefore",initial},{"rendererAfter",measured}};
        QFile file(output+"/"+kind+".json");QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
        qInfo().noquote()<<kind<<"seek CPU p95 ms"<<report["seekSynchronousCpuMs"].toObject()["p95"].toDouble()
                       <<"preparation callback interval p95 ms"<<report["preparationCallbackIntervalMs"].toObject()["p95"].toDouble();
        window.document().undo.setClean();window.close();
    }
};
int main(int argc,char **argv) {return runJesterTests<RenderPerformanceTests>(argc,argv,"JesterPerformanceTests");}
#include "render_performance_tests.moc"
