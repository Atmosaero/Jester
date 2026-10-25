#pragma once
#include "document.h"
#include <QFrame>
#include <QPointer>
class QLabel;
class QListWidget;
class QTextEdit;
class QDoubleSpinBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
namespace trickster {
class Window;
class NotesController;
class NotesCard : public QFrame {
    Q_OBJECT
  public:
    NotesCard(Window *, NotesController *, Document *, QJsonObject note, bool fresh);
    QPointer<Document> document;
    QJsonObject original;
    quint64 structure;
    bool fresh;
    void focusText();
    void refreshAddress();
    bool dirty() const;
  protected:
    bool eventFilter(QObject *, QEvent *) override;
  private:
    Window *window;
    NotesController *controller;
    QLabel *address, *error;
    QTextEdit *text;
    QDoubleSpinBox *start, *end;
    QComboBox *origin, *type;
    QLineEdit *tags;
    QPlainTextEdit *expectations;
    QWidget *advanced;
    QListWidget *bindings;
    bool bindingsEdited = false;
    QJsonArray editedBindings() const;
    QPushButton *attach;
    double initialStart = 0, initialEnd = 0;
    bool attachRequested = false, detachRequested = false;
    int attachFrame = 0;
    QString attachLayer;
    void save();
    void remove();
};
} // namespace trickster
