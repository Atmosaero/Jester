#include "ui/widgets/localized_ui.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLocale>
#include <QRegularExpression>
#include <QPointer>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTableWidget>
#include <QTabWidget>
#include <QWidget>
#include <QApplication>
#include <AzQtComponents/Components/WindowDecorationWrapper.h>

namespace trickster {
QWidget *editorWindowFrame(QWidget *widget) {
    if(widget)if(auto *frame=qobject_cast<AzQtComponents::WindowDecorationWrapper*>(widget->parentWidget());frame && frame->guest()==widget)return frame;
    return widget;
}
QWidget *activeEditorModalWidget() {
    auto *widget=QApplication::activeModalWidget();
    if(auto *frame=qobject_cast<AzQtComponents::WindowDecorationWrapper*>(widget))return frame->guest();
    return widget;
}
namespace {
constexpr int SourceTextRole=Qt::UserRole+319;
QString translated(const QString &source,const QByteArray &context="Jester") {
    const auto bytes=source.toUtf8();return QCoreApplication::translate(context.constData(),bytes.constData());
}
void refreshChoice(QComboBox *combo) {
    const QSignalBlocker blocker(combo);
    const bool custom=combo->isEditable() && combo->currentText()!=combo->itemText(combo->currentIndex());
    const auto customText=combo->currentText();
    for(int i=0;i<combo->count();++i) {
        const auto source=combo->itemData(i,SourceTextRole);
        if(source.isValid())combo->setItemText(i,translated(source.toString()));
    }
    if(custom)combo->setEditText(customText);
}
}
void bindUiText(QObject *object,const char *property,const QString &source,const char *context) {
    const QByteArray effective=QByteArray(property)=="text" && object->metaObject()->indexOfProperty("text")<0 && object->metaObject()->indexOfProperty("plainText")>=0?"plainText":property;
    property=effective.constData();
    if(auto *previous=object->findChild<QObject*>(QString::fromLatin1("_jesterTextFactory_")+property,Qt::FindDirectChildrenOnly))delete previous;
    const auto key=QByteArray("_jesterTranslation_")+property;
    const bool connected=object->property(key.constData()).isValid();
    object->setProperty(key.constData(),QVariantMap{{"source",source},{"context",QString::fromUtf8(context)}});
    if(QByteArray(property)=="text" && qobject_cast<QAction*>(object))object->setProperty("jesterSourceText",source);
    if(!connected)QObject::connect(&LocalizationService::instance(),&LocalizationService::languageChanged,object,[object,key,property=QByteArray(property)] {
        const auto value=object->property(key.constData()).toMap();
        object->setProperty(property.constData(),translated(value["source"].toString(),value["context"].toString().toUtf8()));
    });
    object->setProperty(property,translated(source,context));
}
void bindUiTextFactory(QObject *object,const char *property,std::function<QString()> factory) {
    const QByteArray effective=QByteArray(property)=="text" && object->metaObject()->indexOfProperty("text")<0 && object->metaObject()->indexOfProperty("plainText")>=0?"plainText":property;
    property=effective.constData();
    const auto name=QString::fromLatin1("_jesterTextFactory_")+property;
    if(auto *previous=object->findChild<QObject*>(name,Qt::FindDirectChildrenOnly))delete previous;
    auto *binding=new QObject(object);binding->setObjectName(name);
    auto refresh=[object,property=QByteArray(property),factory=std::move(factory)] {object->setProperty(property.constData(),factory());};
    QObject::connect(&LocalizationService::instance(),&LocalizationService::languageChanged,binding,refresh);refresh();
}
QString roleSourceName(const QString &id) {
    // Only built-in semantic roles have translated labels; custom IDs stay authored.
    static const QMap<QString,QString> names{
        {"root",QT_TRANSLATE_NOOP("Jester","Root")},{"pelvis",QT_TRANSLATE_NOOP("Jester","Pelvis")},
        {"spine",QT_TRANSLATE_NOOP("Jester","Spine")},{"chest",QT_TRANSLATE_NOOP("Jester","Chest")},
        {"neck",QT_TRANSLATE_NOOP("Jester","Neck")},{"head",QT_TRANSLATE_NOOP("Jester","Head")},
        {"leftShoulder",QT_TRANSLATE_NOOP("Jester","Left shoulder")},{"rightShoulder",QT_TRANSLATE_NOOP("Jester","Right shoulder")},
        {"leftUpperArm",QT_TRANSLATE_NOOP("Jester","Left upper arm")},{"rightUpperArm",QT_TRANSLATE_NOOP("Jester","Right upper arm")},
        {"leftForeArm",QT_TRANSLATE_NOOP("Jester","Left forearm")},{"rightForeArm",QT_TRANSLATE_NOOP("Jester","Right forearm")},
        {"leftHand",QT_TRANSLATE_NOOP("Jester","Left hand")},{"rightHand",QT_TRANSLATE_NOOP("Jester","Right hand")},
        {"leftUpperLeg",QT_TRANSLATE_NOOP("Jester","Left thigh")},{"rightUpperLeg",QT_TRANSLATE_NOOP("Jester","Right thigh")},
        {"leftLeg",QT_TRANSLATE_NOOP("Jester","Left shin")},{"rightLeg",QT_TRANSLATE_NOOP("Jester","Right shin")},
        {"leftFoot",QT_TRANSLATE_NOOP("Jester","Left foot")},{"rightFoot",QT_TRANSLATE_NOOP("Jester","Right foot")},
        {"leftToe",QT_TRANSLATE_NOOP("Jester","Left toe")},{"rightToe",QT_TRANSLATE_NOOP("Jester","Right toe")}};
    return names.value(id,id);
}
QMessageBox::StandardButton showUiMessage(QMessageBox::Icon icon,QWidget *parent,std::function<QString()> title,
    std::function<QString()> text,QMessageBox::StandardButtons buttons,QMessageBox::StandardButton defaultButton) {
    QMessageBox dialog(icon,{}, {},buttons,parent);
    bindUiTextFactory(&dialog,"windowTitle",std::move(title));bindUiTextFactory(&dialog,"text",std::move(text));
    if(defaultButton!=QMessageBox::NoButton)dialog.setDefaultButton(defaultButton);
    return static_cast<QMessageBox::StandardButton>(dialog.exec());
}
void setRoleItem(QTableWidget *table,int row,const QString &id) {
    const auto source=roleSourceName(id);
    auto *item=new QTableWidgetItem(source==id?id:uiText(source));item->setData(Qt::UserRole,id);
    item->setFlags(item->flags()&~Qt::ItemIsEditable);table->setItem(row,0,item);
    if(!table->property("jesterRolesBound").toBool()) {
        table->setProperty("jesterRolesBound",true);
        QObject::connect(&LocalizationService::instance(),&LocalizationService::languageChanged,table,[table] {
            const QSignalBlocker blocker(table);
            for(int row=0;row<table->rowCount();++row)if(auto *item=table->item(row,0)) {
                const auto id=item->data(Qt::UserRole).toString(),source=roleSourceName(id);
                item->setText(source==id?id:uiText(source));
            }
        });
    }
}
QString uiSourceText(const QObject *object) {
    if(const auto *action=qobject_cast<const QAction*>(object);action && action->menu())return uiSourceText(action->menu());
    for(const auto *property:{"text","title","windowTitle"}) {
        const auto binding=object->property((QByteArray("_jesterTranslation_")+property).constData());
        if(binding.isValid())return binding.toMap()["source"].toString();
    }
    for(const auto *property:{"text","title","windowTitle"}) {
        auto value=object->property(property);if(value.isValid())return value.toString();
    }
    return {};
}
std::function<void()> preserveUiDrafts(QWidget &root) {
    struct Draft {QPointer<QLineEdit> edit;QString text;int cursor,start,length;bool modified;QLocale locale;QPointer<QAbstractSpinBox> number;QString prefix,suffix;bool floating=false;};
    QList<Draft> drafts;
    for(auto *edit:root.findChildren<QLineEdit*>())if(edit->isModified() || edit->hasFocus()) {
        Draft draft{edit,edit->text(),edit->cursorPosition(),edit->selectionStart(),int(edit->selectedText().size()),edit->isModified(),edit->locale(),qobject_cast<QAbstractSpinBox*>(edit->parentWidget()),{},{},false};
        if(auto *number=qobject_cast<QDoubleSpinBox*>(draft.number.data())) {draft.prefix=number->prefix();draft.suffix=number->suffix();draft.floating=true;}
        if(auto *number=qobject_cast<QSpinBox*>(draft.number.data())) {draft.prefix=number->prefix();draft.suffix=number->suffix();}
        drafts<<draft;
    }
    return [drafts] {
        for(const auto &draft:drafts)if(draft.edit) {
            QSignalBlocker blocker(draft.edit);
            QString text=draft.text;
            if(draft.number && text.startsWith(draft.prefix) && text.endsWith(draft.suffix) && text.size()>=draft.prefix.size()+draft.suffix.size()) {
                QString prefix=draft.prefix,suffix=draft.suffix;
                if(auto *number=qobject_cast<QDoubleSpinBox*>(draft.number.data())) {prefix=number->prefix();suffix=number->suffix();}
                if(auto *number=qobject_cast<QSpinBox*>(draft.number.data())) {prefix=number->prefix();suffix=number->suffix();}
                auto clean=text.mid(draft.prefix.size(),text.size()-draft.prefix.size()-draft.suffix.size());
                if(draft.locale!=draft.number->locale()) {
                    bool valid=false;draft.locale.toDouble(clean,&valid);
                    const QRegularExpression partial("^[+-]?[0-9]*(?:"+QRegularExpression::escape(draft.locale.decimalPoint())+"[0-9]*)?$");
                    if(valid || partial.match(clean).hasMatch()) {
                        clean.remove(draft.locale.groupSeparator());
                        if(draft.floating)clean.replace(draft.locale.decimalPoint(),draft.number->locale().decimalPoint());
                    }
                }
                text=prefix+clean+suffix;
            }
            if(draft.edit->text()!=text)draft.edit->setText(text);
            draft.edit->setModified(draft.modified);draft.edit->setCursorPosition(draft.cursor);
            if(draft.start>=0)draft.edit->setSelection(draft.start,draft.length);
        }
    };
}
void bindToolTip(QWidget *widget,const QString &source) {bindUiText(widget,"toolTip",source);}
void bindPlaceholder(QWidget *widget,const QString &source) {bindUiText(widget,"placeholderText",source);}
QLabel *translatedLabel(const QString &source,QWidget *parent) {
    auto *widget=new QLabel(parent);bindUiText(widget,"text",source);return widget;
}
QPushButton *translatedButton(const QString &source,QWidget *parent) {
    auto *widget=new QPushButton(parent);bindUiText(widget,"text",source);return widget;
}
QCheckBox *translatedCheckBox(const QString &source,QWidget *parent) {
    auto *widget=new QCheckBox(parent);bindUiText(widget,"text",source);return widget;
}
QGroupBox *translatedGroupBox(const QString &source,QWidget *parent) {
    auto *widget=new QGroupBox(parent);bindUiText(widget,"title",source);return widget;
}
QAction *addTranslatedAction(QMenu *menu,const QString &source) {
    auto *action=menu->addAction(QString());bindUiText(action,"text",source);return action;
}
QMenu *addTranslatedMenu(QMenuBar *bar,const QString &source) {
    auto *menu=bar->addMenu(QString());bindUiText(menu,"title",source);return menu;
}
QMenu *addTranslatedMenu(QMenu *parent,const QString &source) {
    auto *menu=parent->addMenu(QString());bindUiText(menu,"title",source);return menu;
}
void addTranslatedRow(QFormLayout *layout,const QString &source,QWidget *field) {layout->addRow(translatedLabel(source,layout->parentWidget()),field);}
void addTranslatedRow(QFormLayout *layout,const QString &source,QLayout *field) {layout->addRow(translatedLabel(source,layout->parentWidget()),field);}
void addTranslatedItem(QComboBox *combo,const QString &source,const QVariant &value) {
    if(!combo->property("jesterTranslatedChoices").toBool()) {
        combo->setProperty("jesterTranslatedChoices",true);
        QObject::connect(&LocalizationService::instance(),&LocalizationService::languageChanged,combo,[combo]{refreshChoice(combo);});
    }
    combo->addItem(translated(source),value.isValid()?value:QVariant(source));
    combo->setItemData(combo->count()-1,source,SourceTextRole);
}
void addTranslatedItems(QComboBox *combo,const QStringList &source) {for(const auto &item:source)addTranslatedItem(combo,item);}
QString choiceText(const QComboBox *combo) {
    const auto source=combo->currentData(SourceTextRole);
    if(combo->isEditable() && combo->currentText()!=combo->itemText(combo->currentIndex()))return combo->currentText();
    return source.isValid()?source.toString():combo->currentText();
}
void setChoiceText(QComboBox *combo,const QString &source) {
    for(int i=0;i<combo->count();++i)
        if(combo->itemData(i,SourceTextRole)==source || combo->itemText(i)==source) {combo->setCurrentIndex(i);return;}
    if(combo->isEditable())combo->setEditText(source);
}
void bindHeaders(QTreeWidget *tree,const QStringList &source) {
    tree->setProperty("jesterSourceHeaders",source);
    if(!tree->property("jesterHeadersBound").toBool()) {
        tree->setProperty("jesterHeadersBound",true);
        QObject::connect(&LocalizationService::instance(),&LocalizationService::languageChanged,tree,[tree] {
            QStringList labels;
            for(const auto &text:tree->property("jesterSourceHeaders").toStringList())labels<<translated(text);
            tree->setHeaderLabels(labels);
        });
    }
    QStringList labels;for(const auto &text:source)labels<<translated(text);tree->setHeaderLabels(labels);
}
void bindHeaders(QTableWidget *table,const QStringList &source) {
    table->setProperty("jesterSourceHeaders",source);
    auto refresh=[table] {
        QStringList labels;for(const auto &source:table->property("jesterSourceHeaders").toStringList())labels<<translated(source);
        table->setHorizontalHeaderLabels(labels);
    };
    if(!table->property("jesterHeadersBound").toBool()) {
        table->setProperty("jesterHeadersBound",true);
        QObject::connect(&LocalizationService::instance(),&LocalizationService::languageChanged,table,refresh);
    }
    refresh();
}
void addTranslatedTab(QTabWidget *tabs,QWidget *page,const QString &source) {
    tabs->addTab(page,translated(source));
    QObject::connect(&LocalizationService::instance(),&LocalizationService::languageChanged,page,[tabs,page,source] {
        const int index=tabs->indexOf(page);if(index>=0)tabs->setTabText(index,translated(source));
    });
}
} // namespace trickster
