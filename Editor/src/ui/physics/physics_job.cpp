#include "ui/widgets/localized_ui.h"
#include "automation/mcp_server.h"
#include "ui/window/window.h"
#include <QCheckBox>
namespace trickster {
Window::~Window() {
    // QObject disconnects receivers after QWidget has deleted children. Our Workspace
    // member is already destroyed by then, so widget teardown must not invoke editor lambdas.
    for(auto object:findChildren<QObject*>())QObject::disconnect(object,nullptr,this,nullptr);
    physicsPoll.stop();
    cancelPhysicsTask();
    if (mcp)
        mcp->stop();
    workspace.disconnect();
    for (auto &d : workspace.documents) {
        d->disconnect();
        d->undo.disconnect();
    }
    // Native viewport teardown can drain engine work. Destroy it while the
    // document members still exist, and before QWidget deletes its children.
    delete viewport;
    viewport=nullptr;
}
void Window::cancelPhysicsTask() {
    physicsJobs.cancel();
}
QJsonObject Window::physicsTaskState() const {
    return physicsJobs.state();
}
void Window::clearPhysicsResult() {
    physicsJobs.clear();
    physicsPreview.reset();
    viewport->physicsPreview.reset();
    viewport->physicsDiagnostics={};
    applyPhysics->setEnabled(false);
    bindUiText(physicsStats,"text","No preview");
    viewport->update();
}
bool Window::startPhysicsTask(bool ballistic, int startFrame, int endFrame, QString &error) {
    return startPhysicsTask(ballistic?"flight":"simulate",startFrame,endFrame,{},error);
}
bool Window::startPhysicsTask(const QString &operation,int startFrame,int endFrame,const QJsonObject &settings,QString &error) {
    auto state=physicsJobs.state();
    if(state["status"]=="running"&&!state["done"].toBool()){error="A calculation is running. Cancel it first.";return false;}
    if(document().scene.nodes.empty()||startFrame<document().scene.first||endFrame>document().scene.last||startFrame>=endFrame){error="Choose at least two frames inside the clip.";return false;}
    if(!settings.isEmpty()&&document().scene.physics.requests[operation].toObject()!=settings)document().edit("Physics calculation settings",[&](Scene &s){s.physics.requests[operation]=settings;},false);
    if (!physicsJobs.start(document(), operation, startFrame, endFrame, settings, error))
        return false;
    physicsPreview.reset();
    viewport->physicsPreview.reset();
    viewport->physicsDiagnostics={};
    applyPhysics->setEnabled(false);
    bindUiText(physicsStats,"text","Calculating…");
    physicsProgress->setValue(0);
    physicsProgress->show();
    cancelPhysics->show();
    physicsPoll.start(30);
    return true;
}
void Window::pollPhysicsTask() {
    physicsJobs.poll();
    auto state = physicsJobs.state();
    auto status = state["status"].toString();
    physicsProgress->setValue(state["progress"].toInt());
    bool running = status == "running" || (!state["done"].toBool() && state["cancelRequested"].toBool());
    physicsProgress->setVisible(running);
    cancelPhysics->setVisible(running);
    if (running)
        return;
    physicsPoll.stop();
    if (hasPhysicsResult() && physicsPreview && viewport->physicsPreview)
        return;
    physicsPreview = physicsJobs.result(document());
    viewport->physicsPreview = physicsPreview;
    viewport->physicsDiagnostics=state["report"].toObject();
    applyPhysics->setEnabled(physicsPreview.has_value());
    bindUiTextFactory(physicsStats,"text",[state,status] {return status == "ready" ? translatedDiagnostic(state["summary"].toString())
                          : status == "stale"
                              ? uiText("Source document changed or closed; result discarded. Recalculate.")
                          : status == "failed"    ? translatedDiagnostic(state["error"].toString())
                          : status == "cancelled" ? uiText("Cancelled. Source motion preserved.")
                                                  : ownedValueLabel(status);});
    if (physicsPreview) {
        viewport->showPhysicsPreview = true;
        viewport->update();
    }
}
} // namespace trickster
