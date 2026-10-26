#include "ui/dialogs/preferences_dialog.h"
#include "ui/window/window.h"
#include "settings/action_registry.h"
#include "settings/preferences_service.h"
#include "ui/widgets/localized_ui.h"
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QScreen>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace trickster {
void showPreferences(Window &window) {
    auto &service=PreferencesService::instance();EditorPreferences baseline=service.current(),draft=baseline;
    auto originalLayout=window.saveState(),originalGeometry=editorWindowFrame(&window)->saveGeometry();
    ActionRegistry registry(window);
    QDialog dialog(&window);dialog.setObjectName("preferencesDialog");bindUiText(&dialog,"windowTitle","Preferences");
    dialog.resize(760,680);auto *layout=new QVBoxLayout(&dialog);
    auto *search=new QLineEdit(&dialog);search->setObjectName("preferencesSearch");bindPlaceholder(search,"Search preferences or actions");layout->addWidget(search);
    auto *scroll=new QScrollArea(&dialog);scroll->setWidgetResizable(true);auto *body=new QWidget(scroll);auto *groups=new QVBoxLayout(body);
    QMap<QString,QFormLayout*> forms;QMap<QString,QWidget*> fields;
    auto group=[&](const QString &title) {
        auto *box=translatedGroupBox(title,body);auto *form=new QFormLayout(box);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        groups->addWidget(box);forms[title]=form;return form;
    };
    auto *appearance=group("Appearance"),*overlay=group("Viewport overlays"),*language=group("Language");
    auto *status=translatedLabel("Preview changes before applying.",&dialog);status->setObjectName("preferencesStatus");status->setWordWrap(true);
    bool refreshing=false;
    auto patch=[&](const QString &path,const QJsonValue &value) {
        if(refreshing)return;
        const auto parts=path.split('.');QJsonObject changes;
        if(parts.size()==1)changes[parts[0]]=value;else changes[parts[0]]=QJsonObject{{parts[1],value}};
        QString error;auto next=draft;
        if(!next.patch(changes,error)) {bindUiTextFactory(status,"text",[error] {return translatedDiagnostic(error);});return;}
        draft=next;
    };
    auto combo=[&](QFormLayout *form,const QString &path,const QString &label,const QList<QPair<QString,QString>> &choices) {
        auto *widget=new QComboBox(body);widget->setObjectName("preference_"+path);
        for(const auto &[text,value]:choices)addTranslatedItem(widget,text,value);
        widget->setCurrentIndex(widget->findData(draft.value(path).toVariant()));fields[path]=widget;
        addTranslatedRow(form,label,widget);
        QObject::connect(widget,&QComboBox::currentIndexChanged,&dialog,[&,path,widget]{patch(path,QJsonValue::fromVariant(widget->currentData()));});
    };
    auto number=[&](const QString &path,const QString &label,double low,double high,double step) {
        auto *widget=new QDoubleSpinBox(body);widget->setObjectName("preference_"+path);widget->setRange(low,high);widget->setSingleStep(step);
        widget->setDecimals(step>=1?0:2);widget->setKeyboardTracking(false);widget->setValue(draft.value(path).toDouble());fields[path]=widget;
        addTranslatedRow(appearance,label,widget);QObject::connect(widget,&QDoubleSpinBox::valueChanged,&dialog,[&,path](double value){patch(path,value);});
    };
    auto color=[&](QFormLayout *form,const QString &path,const QString &label) {
        auto *widget=new QPushButton(draft.value(path).toString(),body);widget->setObjectName("preference_"+path);fields[path]=widget;addTranslatedRow(form,label,widget);
        QObject::connect(widget,&QPushButton::clicked,&dialog,[&,path,widget] {
            auto value=QColorDialog::getColor(QColor(draft.value(path).toString()),&dialog,uiText("Choose color"));
            if(value.isValid()) {patch(path,value.name());widget->setText(value.name());}
        });
    };
    auto *font=new QFontComboBox(body);font->setWritingSystem(QFontDatabase::Cyrillic);font->setObjectName("preference_appearance.fontFamily");font->setCurrentFont(QFont(draft.value("appearance.fontFamily").toString()));
    fields["appearance.fontFamily"]=font;addTranslatedRow(appearance,"Font family",font);
    QObject::connect(font,&QFontComboBox::currentFontChanged,&dialog,[&](const QFont &value){patch("appearance.fontFamily",value.family());});
    number("appearance.fontSize","Font size (pt)",7,32,1);number("appearance.uiScale","Interface scale",.75,2.5,.05);number("appearance.iconSize","Icon size (px)",12,48,1);
    combo(appearance,"appearance.density","Density",{{"Compact","compact"},{"Comfortable","comfortable"}});
    combo(appearance,"appearance.theme","Theme",{{"Dark","dark"},{"Light","light"},{"High contrast","high-contrast"}});
    color(appearance,"appearance.accent","Accent color");
    for(const auto &[path,label]:QList<QPair<QString,QString>>{{"bones","Skeleton"},{"selection","Selection"},{"notes","Notes"},{"trajectory","Trajectory"},{"manipulatorX","Manipulator X"},{"manipulatorY","Manipulator Y"},{"manipulatorZ","Manipulator Z"}})
        color(overlay,"overlays."+path,label);
    combo(language,"language","Interface language",{{"System","system"},{"English","en"},{"Russian","ru"}});
    auto *actionsGroup=translatedGroupBox("Keyboard shortcuts",body);groups->addWidget(actionsGroup);auto *actionLayout=new QVBoxLayout(actionsGroup);
    auto *actions=new QTreeWidget(actionsGroup);actions->setObjectName("preferencesActions");bindHeaders(actions,{"Action","Shortcut","Stable ID"});
    auto *actionCount=new QLabel(actionsGroup);actionCount->setObjectName("preferencesActionCount");actionLayout->addWidget(actionCount);
    actions->setRootIsDecorated(false);actions->setMinimumHeight(180);actions->header()->setSectionResizeMode(QHeaderView::ResizeToContents);actionLayout->addWidget(actions);
    auto *key=new QKeySequenceEdit(actionsGroup);key->setObjectName("preferencesShortcutEdit");actionLayout->addWidget(key);
    auto *setKey=translatedButton("Assign shortcut",actionsGroup),*resetKey=translatedButton("Reset action shortcut",actionsGroup);
    auto *actionRow=new QHBoxLayout;actionRow->addWidget(setKey);actionRow->addWidget(resetKey);actionLayout->addLayout(actionRow);
    auto refreshActions=[&] {
        const auto selected=actions->currentItem()?actions->currentItem()->text(2):QString();actions->clear();
        const auto overrides=draft.toJson()["shortcuts"].toObject();
        for(auto *action:registry.actions()) {
            const auto id=ActionRegistry::id(action);QStringList keys;
            if(overrides.contains(id))keys<<overrides[id].toString();else keys=action->property("jesterDefaultShortcuts").toStringList();
            auto *item=new QTreeWidgetItem(actions,{action->text(),keys.join(", "),id});
            if(id==selected)actions->setCurrentItem(item);
        }
        actionCount->setText(uiText("%n action(s)",actions->topLevelItemCount()));
    };refreshActions();
    QObject::connect(actions,&QTreeWidget::currentItemChanged,&dialog,[&](QTreeWidgetItem *item) {
        key->setKeySequence(item?QKeySequence::fromString(item->text(1).section(", ",0,0),QKeySequence::PortableText):QKeySequence());
    });
    QObject::connect(setKey,&QPushButton::clicked,&dialog,[&] {
        if(!actions->currentItem())return;auto shortcuts=draft.toJson()["shortcuts"].toObject();
        shortcuts[actions->currentItem()->text(2)]=key->keySequence().toString(QKeySequence::PortableText);
        auto next=draft;QString error;
        if(!next.patch({{"shortcuts",shortcuts}},error) || !registry.validate(next,error)) {bindUiTextFactory(status,"text",[error] {return translatedDiagnostic(error);});return;}
        draft=next;refreshActions();status->setText(uiText("Shortcut assigned."));
    });
    QObject::connect(resetKey,&QPushButton::clicked,&dialog,[&] {
        if(!actions->currentItem())return;auto shortcuts=draft.toJson()["shortcuts"].toObject();shortcuts.remove(actions->currentItem()->text(2));
        QString error;draft.patch({{"shortcuts",shortcuts}},error);refreshActions();
    });
    auto *layouts=group("Workspace layouts");auto *layoutList=new QComboBox(body);layoutList->setObjectName("preferencesLayouts");
    auto refreshLayouts=[&] {layoutList->clear();for(const auto &name:draft.toJson()["layouts"].toObject().keys())layoutList->addItem(name);};refreshLayouts();
    addTranslatedRow(layouts,"Saved layout",layoutList);
    auto *layoutButtons=new QWidget(body);auto *layoutRow=new QHBoxLayout(layoutButtons);layoutRow->setContentsMargins(0,0,0,0);
    for(const auto &label:{QString("Save current layout"),QString("Restore layout"),QString("Delete layout")}) {
        auto *button=translatedButton(label,layoutButtons);layoutRow->addWidget(button);
        QObject::connect(button,&QPushButton::clicked,&dialog,[&,label] {
            auto values=draft.toJson()["layouts"].toObject();QString error;
            if(label=="Save current layout") {
                bool ok=false;auto name=QInputDialog::getText(&dialog,uiText("Save layout"),uiText("Name"),QLineEdit::Normal,{},&ok).trimmed();if(!ok || name.isEmpty())return;
                values[name]=QString::fromLatin1(window.saveState().toBase64());
            } else if(label=="Restore layout") {
                auto value=values[layoutList->currentText()].toString();if(value.isEmpty())return;
                if(!window.restoreState(QByteArray::fromBase64(value.toLatin1())))status->setText(uiText("Cannot restore this layout."));return;
            } else values.remove(layoutList->currentText());
            if(!draft.patch({{"layouts",values}},error))bindUiTextFactory(status,"text",[error] {return translatedDiagnostic(error);});refreshLayouts();
        });
    }
    layouts->addRow(layoutButtons);groups->addStretch();scroll->setWidget(body);layout->addWidget(scroll,1);layout->addWidget(status);
    auto refreshFields=[&] {
        refreshing=true;
        for(auto it=fields.begin();it!=fields.end();++it) {
            QSignalBlocker block(it.value());const auto value=draft.value(it.key());
            if(auto *number=qobject_cast<QDoubleSpinBox*>(it.value()))number->setValue(value.toDouble());
            if(auto *font=qobject_cast<QFontComboBox*>(it.value()))font->setCurrentFont(QFont(value.toString()));
            else if(auto *combo=qobject_cast<QComboBox*>(it.value()))combo->setCurrentIndex(combo->findData(value.toVariant()));
            if(auto *button=qobject_cast<QPushButton*>(it.value()))button->setText(value.toString());
        }
        refreshActions();refreshLayouts();refreshing=false;
    };
    auto *fileRow=new QGridLayout;layout->addLayout(fileRow);
    auto *importButton=translatedButton("Import preferences",&dialog),*exportButton=translatedButton("Export preferences",&dialog),*reset=translatedButton("Reset all preferences",&dialog),*preview=translatedButton("Preview",&dialog);
    preview->setObjectName("preferencesPreview");
    fileRow->addWidget(importButton,0,0);fileRow->addWidget(exportButton,0,1);fileRow->addWidget(reset,1,0);fileRow->addWidget(preview,1,1);
    QObject::connect(importButton,&QPushButton::clicked,&dialog,[&] {
        auto path=QFileDialog::getOpenFileName(&dialog,uiText("Import preferences"),{},uiText("Jester preferences (*.json)"));if(path.isEmpty())return;
        EditorPreferences next;QString error;if(!EditorPreferences::load(path,next,error) || !registry.validate(next,error)) {bindUiTextFactory(status,"text",[error] {return translatedDiagnostic(error);});return;}
        draft=next;refreshFields();
    });
    QObject::connect(exportButton,&QPushButton::clicked,&dialog,[&] {
        auto path=QFileDialog::getSaveFileName(&dialog,uiText("Export preferences"),"jester-preferences.json",uiText("Jester preferences (*.json)"));if(path.isEmpty())return;
        QString error;if(!draft.save(path,error))bindUiTextFactory(status,"text",[error] {return translatedDiagnostic(error);});
    });
    QObject::connect(reset,&QPushButton::clicked,&dialog,[&] {draft=EditorPreferences();refreshFields();});
    auto applyPreview=[&] {
        QString error;if(!registry.validate(draft,error)) {bindUiTextFactory(status,"text",[error] {return translatedDiagnostic(error);});return false;}
        if(!service.preview(draft,error)) {bindUiTextFactory(status,"text",[error] {return translatedDiagnostic(error);});return false;}
        registry.apply(draft);return true;
    };
    QObject::connect(preview,&QPushButton::clicked,&dialog,[&]{if(applyPreview())bindUiTextFactory(status,"text",[] {return uiText("Preview active. Cancel restores your previous settings.");});});
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Apply|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);
    auto applyDraft=[&] {if(!applyPreview())return false;QString error;if(!service.apply(draft,error)) {bindUiTextFactory(status,"text",[error] {return translatedDiagnostic(error);});return false;}baseline=draft;originalLayout=window.saveState();originalGeometry=editorWindowFrame(&window)->saveGeometry();return true;};
    QObject::connect(buttons->button(QDialogButtonBox::Apply),&QPushButton::clicked,&dialog,[&]{applyDraft();});
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{if(applyDraft())dialog.accept();});
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(search,&QLineEdit::textChanged,&dialog,[&](const QString &text) {
        for(auto *form:forms) {
            bool any=false;
            for(int row=0;row<form->rowCount();++row) {
                auto *item=form->itemAt(row,QFormLayout::LabelRole);auto *label=item?qobject_cast<QLabel*>(item->widget()):nullptr;
                bool visible=text.isEmpty() || (label && label->text().contains(text,Qt::CaseInsensitive));form->setRowVisible(row,visible);any|=visible;
            }
            form->parentWidget()->setVisible(any);
        }
        bool any=false;for(int i=0;i<actions->topLevelItemCount();++i) {
            auto *item=actions->topLevelItem(i);const bool visible=(item->text(0)+" "+item->text(1)+" "+item->text(2)).contains(text,Qt::CaseInsensitive);
            item->setHidden(!visible);any|=visible;
        }
        actionsGroup->setVisible(any || text.isEmpty());
    });
    QObject::connect(&LocalizationService::instance(),&LocalizationService::languageChanged,&dialog,[&]{const QSignalBlocker preservePendingShortcut(actions);refreshActions();});
    const auto available=window.screen()->availableGeometry().size()-QSize(24,48);
    dialog.setMaximumSize(available);dialog.resize(QSize(760,680).boundedTo(available));
    dialog.exec();QString ignored;service.preview(baseline,ignored);registry.apply(baseline);
    if(dialog.result()!=QDialog::Accepted) {editorWindowFrame(&window)->restoreGeometry(originalGeometry);window.restoreState(originalLayout);}
}
}
