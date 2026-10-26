#include "ui/window/scene_settings_panel.h"
#include "ui/widgets/localized_ui.h"
#include "settings/preferences_service.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace trickster {
SceneSettingsPanel::SceneSettingsPanel(Workspace &source,std::function<void()> frameCamera,QWidget *parent)
    :QWidget(parent),workspace(source) {
    setObjectName("sceneSettingsPanel");auto *layout=new QVBoxLayout(this);
    layout->setContentsMargins(4,4,4,4);
    auto *search=new QLineEdit(this);search->setObjectName("sceneSettingsSearch");bindPlaceholder(search,"Search scene settings");layout->addWidget(search);
    auto *presetRow=new QHBoxLayout;auto *presets=new QComboBox(this);presets->setObjectName("sceneVisualPreset");
    addTranslatedItem(presets,"Neutral","neutral");addTranslatedItem(presets,"Studio","studio");addTranslatedItem(presets,"Silhouette","silhouette");
    auto *apply=translatedButton("Apply preset",this),*save=translatedButton("Save preset",this);
    presetRow->addWidget(presets,1);presetRow->addWidget(apply);presetRow->addWidget(save);layout->addLayout(presetRow);
    // Presets contain authored visual values only, never private project paths.
    const auto custom=PreferencesService::instance().current().toJson()["scenePresets"].toObject().toVariantMap();
    for(auto it=custom.begin();it!=custom.end();++it)presets->addItem(it.key(),it.value());
    connect(apply,&QPushButton::clicked,this,[this,presets] {
        SceneVisualSettings next;QString error;
        if(presets->currentData().typeId()==QMetaType::QString)next=SceneVisualSettings::preset(presets->currentData().toString());
        else if(!SceneVisualSettings::fromJson(QJsonObject::fromVariantMap(presets->currentData().toMap()),next,error)) {emit workspace.feedback(error);return;}
        workspace.active().editVisual("Scene preset",next);
    });
    connect(save,&QPushButton::clicked,this,[this,presets] {
        bool ok=false;auto name=QInputDialog::getText(this,uiText("Save scene preset"),uiText("Name"),QLineEdit::Normal,{},&ok).trimmed();
        if(!ok || name.isEmpty())return;
        auto preferences=PreferencesService::instance().current();auto custom=preferences.toJson()["scenePresets"].toObject().toVariantMap();
        auto value=workspace.active().scene.visuals.toJson().toVariantMap();custom[name]=value;QString error;
        if(!preferences.patch({{"scenePresets",QJsonObject::fromVariantMap(custom)}},error) || !PreferencesService::instance().apply(preferences,error)) {emit workspace.feedback(error);return;}
        int index=presets->findText(name);if(index<0) {presets->addItem(name,value);index=presets->count()-1;}else presets->setItemData(index,value);
        presets->setCurrentIndex(index);
    });
    auto *scroll=new QScrollArea(this);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    auto *body=new QWidget(scroll);auto *groups=new QVBoxLayout(body);groups->setContentsMargins(0,0,0,0);
    QMap<QString,QFormLayout*> forms;
    for(const auto &field:SceneVisualSettings::fields()) {
        const QString group=QString::fromUtf8(field.group);
        if(!forms.contains(group)) {
            auto *box=new QGroupBox(body);bindUiText(box,"title",group,"SceneSettings");box->setProperty("searchSource",group);
            auto *form=new QFormLayout(box);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            form->setRowWrapPolicy(QFormLayout::WrapLongRows);forms[group]=form;groups->addWidget(box);
        }
        QWidget *control=nullptr;QWidget *row=nullptr;
        const auto path=field.path;
        if(field.kind==VisualFieldKind::Toggle) {
            auto *check=new QCheckBox(body);control=check;
            connect(check,&QCheckBox::toggled,this,[this,path](bool checked) {change(path,checked);});
            if(path=="lighting.fill.shadows") {check->setEnabled(false);bindToolTip(check,"Atom supports one directional shadow caster; use the main light.");}
        } else if(field.kind==VisualFieldKind::Choice) {
            auto *combo=new QComboBox(body);control=combo;
            for(const auto &choice:field.choices) {
                const QMap<QString,QString> labels{{"lit","Lit"},{"unlit","Unlit"},{"wireframe","Wireframe"},{"low","Low"},{"medium","Medium"},{"high","High"},
                    {"solid","Solid"},{"gradient","Gradient"},{"perspective","Perspective"},{"orthographic","Orthographic"},
                    {"neutral-studio","Neutral studio"},{"aces","ACES"},{"reinhard","Reinhard"},{"srgb","Display sRGB"}};
                addTranslatedItem(combo,labels.value(choice,choice),choice);
            }
            connect(combo,&QComboBox::currentIndexChanged,this,[this,path,combo] {change(path,QJsonValue::fromVariant(combo->currentData()));});
        } else if(field.kind==VisualFieldKind::Color) {
            auto *button=new QPushButton(body);control=button;
            connect(button,&QPushButton::clicked,this,[this,path] {
                auto color=QColorDialog::getColor(QColor(workspace.active().scene.visuals.value(path).toString()),this,uiText("Choose color"));
                if(color.isValid())change(path,color.name());
            });
        } else {
            auto *number=new QDoubleSpinBox(body);control=number;number->setRange(field.minimum,field.maximum);
            number->setSingleStep(field.step);number->setDecimals(field.step>=1?0:3);number->setKeyboardTracking(false);
            number->setMinimumWidth(70);
            row=new QWidget(body);auto *line=new QHBoxLayout(row);line->setContentsMargins(0,0,0,0);line->addWidget(number);
            if(field.maximum-field.minimum<=400 || path.endsWith("intensityLux")) {
                auto *slider=new QSlider(Qt::Horizontal,row);slider->setRange(0,1000);slider->setObjectName("sceneSlider_"+path);line->addWidget(slider,1);
                slider->setProperty("visualPath",path);slider->setProperty("minimumValue",field.minimum);slider->setProperty("maximumValue",field.maximum);
                connect(slider,&QSlider::sliderPressed,this,[this] {static int token=1;gesture=++token;});
                connect(slider,&QSlider::sliderReleased,this,[this] {gesture=0;});
                connect(slider,&QSlider::valueChanged,this,[this,path,slider,low=field.minimum,high=field.maximum,number](int value) {
                    if(refreshing)return;const double next=low+(high-low)*value/1000.;
                    {QSignalBlocker block(number);number->setValue(next);}change(path,next,slider->isSliderDown()?gesture:0);
                });
            }
            connect(number,&QDoubleSpinBox::valueChanged,this,[this,path](double value) {change(path,value);});
        }
        control->setObjectName("sceneSetting_"+path);controls[path]=control;
        auto *label=new QLabel(body);bindUiText(label,"text",QString::fromUtf8(field.label),"SceneSettings");label->setWordWrap(true);
        auto *form=forms[group];form->addRow(label,row?row:control);
        (row?row:control)->setProperty("searchSource",group+" "+QString::fromUtf8(field.label));
    }
    auto *frame=translatedButton("Frame character",body);frame->setObjectName("sceneFrameCamera");groups->addWidget(frame);
    connect(frame,&QPushButton::clicked,this,std::move(frameCamera));
    auto *resetPost=translatedButton("Reset post-processing",body);groups->addWidget(resetPost);
    connect(resetPost,&QPushButton::clicked,this,[this] {
        auto next=workspace.active().scene.visuals;QString error;
        next.patch({{"post",SceneVisualSettings().toJson()["post"]}},error);workspace.active().editVisual("Reset post-processing",next);
    });
    auto *reset=translatedButton("Reset scene settings",body);groups->addWidget(reset);
    connect(reset,&QPushButton::clicked,this,[this] {workspace.active().editVisual("Reset scene settings",SceneVisualSettings());});
    groups->addStretch();scroll->setWidget(body);layout->addWidget(scroll,1);
    connect(search,&QLineEdit::textChanged,this,[forms](const QString &text) {
        for(auto *form:forms) {
            bool any=false;
            for(int row=0;row<form->rowCount();++row)if(auto *item=form->itemAt(row,QFormLayout::FieldRole))if(auto *widget=item->widget()) {
                auto *label=qobject_cast<QLabel*>(form->itemAt(row,QFormLayout::LabelRole)->widget());
                const auto *box=qobject_cast<QGroupBox*>(form->parentWidget());
                bool match=text.isEmpty() || (widget->property("searchSource").toString()+" "+label->text()+" "+box->title()).contains(text,Qt::CaseInsensitive);
                form->setRowVisible(row,match);any|=match;
            }
            form->parentWidget()->setVisible(any);
        }
    });
    connect(&workspace,&Workspace::changed,this,[this]{refresh();});
    connect(&workspace,&Workspace::activeChanged,this,[this]{refresh();});refresh();
}
void SceneSettingsPanel::change(const QString &path,const QJsonValue &value,int token) {
    if(refreshing)return;
    auto settings=workspace.active().scene.visuals;QString error;
    if(!settings.setValue(path,value,error)) {emit workspace.feedback(error);return;}
    workspace.active().editVisual("Scene settings",settings,token);
}
void SceneSettingsPanel::refresh() {
    const auto &document=workspace.active();
    if(displayedDocument==document.id && displayedRevision==document.visualRevision)return;
    displayedDocument=document.id;displayedRevision=document.visualRevision;
    refreshing=true;const auto &settings=workspace.active().scene.visuals;
    for(auto it=controls.begin();it!=controls.end();++it) {
        const QSignalBlocker blocker(it.value());auto value=settings.value(it.key());
        if(auto *check=qobject_cast<QCheckBox*>(it.value()))check->setChecked(value.toBool());
        if(auto *combo=qobject_cast<QComboBox*>(it.value()))combo->setCurrentIndex(combo->findData(value.toVariant()));
        if(auto *number=qobject_cast<QDoubleSpinBox*>(it.value()))number->setValue(value.toDouble());
        if(auto *button=qobject_cast<QPushButton*>(it.value())) {button->setText(value.toString());button->setStyleSheet("background:"+value.toString()+";color:"+(QColor(value.toString()).lightnessF()>.5?"black":"white")+";");}
    }
    for(auto *slider:findChildren<QSlider*>()) {
        QSignalBlocker blocker(slider);const auto low=slider->property("minimumValue").toDouble(),high=slider->property("maximumValue").toDouble();
        slider->setValue(qRound((settings.value(slider->property("visualPath").toString()).toDouble()-low)/(high-low)*1000));
    }
    const bool lit=settings.value("mode")=="lit",post=lit && settings.value("post.enabled").toBool();
    auto available=[&](const QString &path) {
        if(path=="lighting.fill.shadows")return false;
        if(path.startsWith("lighting.main.") && path!="lighting.main.enabled")
            return lit && settings.value("lighting.main.enabled").toBool() && (!path.endsWith("shadowQuality") || settings.value("lighting.main.shadows").toBool());
        if(path.startsWith("lighting.fill.") && path!="lighting.fill.enabled")return lit && settings.value("lighting.fill.enabled").toBool();
        if(path.startsWith("lighting.") || path=="environment.enabled")return lit;
        if(path.startsWith("environment."))return lit && settings.value("environment.enabled").toBool();
        if(path=="post.enabled")return lit;
        if(path.startsWith("post.bloom.") && path!="post.bloom.enabled")return post && settings.value("post.bloom.enabled").toBool();
        if(path.startsWith("post."))return post;
        if(path=="camera.fovDegrees")return settings.value("camera.projection")=="perspective";
        if(path=="camera.orthographicHeight")return settings.value("camera.projection")=="orthographic";
        if(path=="background.bottomColor")return settings.value("background.mode")=="gradient";
        if(path.startsWith("wireframe."))return settings.value("mode")=="wireframe";
        if(path=="grid.step" || path=="grid.intensity")return settings.value("grid.enabled").toBool();
        if(path.startsWith("floor.") && path!="floor.enabled")return settings.value("floor.enabled").toBool() && (path!="floor.height" || !settings.value("floor.followPhysicsGround").toBool());
        return true;
    };
    for(auto it=controls.begin();it!=controls.end();++it)it.value()->setEnabled(available(it.key()));
    for(auto *slider:findChildren<QSlider*>())slider->setEnabled(available(slider->property("visualPath").toString()));
    refreshing=false;
}
}
