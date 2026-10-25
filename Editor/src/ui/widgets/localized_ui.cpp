#include "ui/widgets/localized_ui.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QWidget>

namespace trickster {
namespace {
constexpr int SourceTextRole=Qt::UserRole+319;
QString translated(const QString &source,const QByteArray &context="Jester") {
    const auto bytes=source.toUtf8();return QCoreApplication::translate(context.constData(),bytes.constData());
}
void refreshChoice(QComboBox *combo) {
    const QSignalBlocker blocker(combo);
    for(int i=0;i<combo->count();++i) {
        const auto source=combo->itemData(i,SourceTextRole);
        if(source.isValid())combo->setItemText(i,translated(source.toString()));
    }
}
}
void bindUiText(QObject *object,const char *property,const QString &source,const char *context) {
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
} // namespace trickster
