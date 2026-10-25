#pragma once
#include "settings/localization.h"
#include <QStringList>
#include <QVariant>
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
namespace trickster {
void bindUiText(QObject *,const char *property,const QString &source,const char *context="Jester");
void bindToolTip(QWidget *,const QString &source);
void bindPlaceholder(QWidget *,const QString &source);
QLabel *translatedLabel(const QString &source,QWidget *parent=nullptr);
QPushButton *translatedButton(const QString &source,QWidget *parent=nullptr);
QCheckBox *translatedCheckBox(const QString &source,QWidget *parent=nullptr);
QGroupBox *translatedGroupBox(const QString &source,QWidget *parent=nullptr);
QAction *addTranslatedAction(QMenu *,const QString &source);
QMenu *addTranslatedMenu(QMenuBar *,const QString &source);
QMenu *addTranslatedMenu(QMenu *,const QString &source);
void addTranslatedRow(QFormLayout *,const QString &source,QWidget *field);
void addTranslatedRow(QFormLayout *,const QString &source,QLayout *field);
void addTranslatedItem(QComboBox *,const QString &source,const QVariant &value={});
void addTranslatedItems(QComboBox *,const QStringList &source);
QString choiceText(const QComboBox *);
void setChoiceText(QComboBox *,const QString &source);
void bindHeaders(QTreeWidget *,const QStringList &source);
} // namespace trickster
