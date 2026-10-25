#pragma once
#include "core/scene/workspace.h"
#include <QWidget>
#include <functional>
class QFormLayout;
namespace trickster {
class Window;
void installSceneSettings(Window &);
class SceneSettingsPanel final:public QWidget {
  public:
    SceneSettingsPanel(Workspace &,std::function<void()> frameCamera,QWidget *parent=nullptr);
    void refresh();
  private:
    Workspace &workspace;
    QMap<QString,QWidget*> controls;
    bool refreshing=false;
    int gesture=0;
    void change(const QString &path,const QJsonValue &value,int token=0);
};
}
