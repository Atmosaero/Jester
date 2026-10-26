#include "ui/window/scene_settings_panel.h"
#include "ui/window/window.h"
#include "ui/widgets/localized_ui.h"
#include "ui/widgets/icons.h"
#include <QActionGroup>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace trickster {
void installSceneSettings(Window &window) {
    auto *dock=new QDockWidget(&window);dock->setObjectName("sceneSettingsDock");bindUiText(dock,"windowTitle","Scene");
    dock->setWindowIcon(icon(Glyph::Mesh));
    dock->setWidget(new SceneSettingsPanel(window.workspace,[&window] {window.viewport->frameCharacter();},dock));
    dock->setMinimumWidth(270);window.addDockWidget(Qt::RightDockWidgetArea,dock);
    if(auto *inspector=window.findChild<QDockWidget*>("inspectorDock")) {window.tabifyDockWidget(inspector,dock);inspector->raise();}
    auto *menu=window.findChild<QMenu*>("sceneMenu");
    if(!menu) {menu=addTranslatedMenu(window.menuBar(),"Scene");menu->setObjectName("sceneMenu");}
    menu->addSeparator();
    auto *toggle=dock->toggleViewAction();toggle->setObjectName("toggleSceneSettings");bindUiText(toggle,"text","Scene settings");menu->addAction(toggle);
    menu->addSeparator();
    auto *header=new QWidget(&window);header->setObjectName("viewportHeader");
    auto *row=new QHBoxLayout(header);row->setContentsMargins(5,2,5,2);row->setSpacing(3);
    auto *group=new QActionGroup(&window);group->setExclusive(true);
    const QStringList modes{"lit","unlit","wireframe"},labels{"Lit","Unlit","Wireframe"};
    const Glyph glyphs[]{Glyph::Lit,Glyph::Unlit,Glyph::Wireframe};
    for(int i=0;i<modes.size();++i) {
        auto *action=addTranslatedAction(menu,labels[i]);action->setObjectName("viewportMode_"+modes[i]);
        action->setCheckable(true);action->setShortcut(QKeySequence("Alt+"+QString::number(i+1)));group->addAction(action);window.addAction(action);
        action->setProperty("visualMode",modes[i]);
        action->setIcon(icon(glyphs[i]));
        auto *button=new QToolButton(header);button->setDefaultAction(action);button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setObjectName("viewportModeButton_"+modes[i]);
        button->setProperty("jesterTransportButton",true);
        bindUiText(button,"accessibleName",labels[i]);
        bindUiTextFactory(button,"toolTip",[action] {return action->text()+" ("+action->shortcut().toString(QKeySequence::NativeText)+")";});
        row->addWidget(button);
        QObject::connect(action,&QAction::triggered,&window,[&window,mode=modes[i]] {
            auto settings=window.document().scene.visuals;QString error;
            if(settings.setValue("mode",mode,error))window.document().editVisual("Display mode",settings);
        });
    }
    auto refresh=[&window,group] {
        const auto mode=window.document().scene.visuals.value("mode").toString();
        for(auto *action:group->actions())action->setChecked(action->property("visualMode")==mode);
    };
    QObject::connect(&window.workspace,&Workspace::changed,header,refresh);
    QObject::connect(&window.workspace,&Workspace::activeChanged,header,refresh);refresh();row->addStretch();
    auto *sceneButton=new QToolButton(header);bindUiText(sceneButton,"text","Scene");row->addWidget(sceneButton);
    QObject::connect(sceneButton,&QToolButton::clicked,dock,[dock] {dock->show();dock->raise();});
    // Keep native surface and overlay siblings inside the viewport, with a
    // compact Qt header above it. Do not overlay a control on the native HWND.
    auto *parent=window.viewport->parentWidget();auto *parentLayout=parent->layout();
    auto *container=new QWidget(parent);auto *column=new QVBoxLayout(container);column->setContentsMargins(0,0,0,0);column->setSpacing(0);
    delete parentLayout->replaceWidget(window.viewport,container);
    column->addWidget(header);column->addWidget(window.viewport,1);
}
}
