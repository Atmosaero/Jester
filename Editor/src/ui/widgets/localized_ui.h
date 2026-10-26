#pragma once
#include "settings/localization.h"
#include <QStringList>
#include <QVariant>
#include <utility>
#include <functional>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QMessageBox>
class QObject;
class QWidget;
class QLabel;
class QPushButton;
class QCheckBox;
class QGroupBox;
class QAction;
class QMenu;
class QMenuBar;
class QFormLayout;
class QLayout;
class QComboBox;
class QTreeWidget;
class QTableWidget;
class QTabWidget;
class QIcon;
class QDialogButtonBox;
namespace trickster {
void bindUiText(QObject *,const char *property,const QString &source,const char *context="Jester");
void bindUiTextFactory(QObject *,const char *property,std::function<QString()>);
QString roleSourceName(const QString &id);
void setRoleItem(QTableWidget *,int row,const QString &id);
QMessageBox::StandardButton showUiMessage(QMessageBox::Icon,QWidget *,std::function<QString()> title,
    std::function<QString()> text,QMessageBox::StandardButtons buttons=QMessageBox::Ok,
    QMessageBox::StandardButton defaultButton=QMessageBox::NoButton);
QString uiSourceText(const QObject *);
// Snapshot only in-progress text edits. Read-only labels and translated
// choices are free to refresh; restoring drafts never emits edit signals.
std::function<void()> preserveUiDrafts(QWidget &);
template<class T,class... Args> T *translatedWidget(const QString &source,Args&&... args) {
    auto *widget=new T(std::forward<Args>(args)...);
    const char *property=widget->metaObject()->indexOfProperty("text")>=0?"text":
        widget->metaObject()->indexOfProperty("title")>=0?"title":"windowTitle";
    bindUiText(widget,property,source);return widget;
}
template<class T,class... Args> T *translatedIconWidget(const QIcon &icon,const QString &source,Args&&... args) {
    auto *widget=new T(icon,QString(),std::forward<Args>(args)...);bindUiText(widget,"text",source);return widget;
}
void bindToolTip(QWidget *,const QString &source);
void bindPlaceholder(QWidget *,const QString &source);
QLabel *translatedLabel(const QString &source,QWidget *parent=nullptr);
QPushButton *translatedButton(const QString &source,QWidget *parent=nullptr);
QCheckBox *translatedCheckBox(const QString &source,QWidget *parent=nullptr);
QGroupBox *translatedGroupBox(const QString &source,QWidget *parent=nullptr);
QAction *addTranslatedAction(QMenu *,const QString &source);
template<class Menu,class... Args> QAction *addTranslatedAction(Menu *menu,const QString &source,Args&&... args) {
    auto *action=menu->addAction(QString(),std::forward<Args>(args)...);
    bindUiText(action,"text",source);return action;
}
template<class Menu,class... Args> QAction *addTranslatedIconAction(Menu *menu,const QIcon &icon,const QString &source,Args&&... args) {
    auto *action=menu->addAction(icon,QString(),std::forward<Args>(args)...);
    bindUiText(action,"text",source);return action;
}
template<class Role> QPushButton *addTranslatedButton(QDialogButtonBox *box,const QString &source,Role role) {
    auto *button=box->addButton(QString(),role);bindUiText(button,"text",source);return button;
}
QMenu *addTranslatedMenu(QMenuBar *,const QString &source);
QMenu *addTranslatedMenu(QMenu *,const QString &source);
void addTranslatedRow(QFormLayout *,const QString &source,QWidget *field);
void addTranslatedRow(QFormLayout *,const QString &source,QLayout *field);
void addTranslatedItem(QComboBox *,const QString &source,const QVariant &value={});
void addTranslatedItems(QComboBox *,const QStringList &source);
QString choiceText(const QComboBox *);
void setChoiceText(QComboBox *,const QString &source);
void bindHeaders(QTreeWidget *,const QStringList &source);
void bindHeaders(QTableWidget *,const QStringList &source);
void addTranslatedTab(QTabWidget *,QWidget *,const QString &source);
} // namespace trickster
