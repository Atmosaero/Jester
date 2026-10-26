#include "settings/editor_appearance.h"
#include "settings/preferences_service.h"
#include "settings/blue_jay_style.h"
#include "ui/widgets/localized_ui.h"
#include "ui/widgets/icons.h"
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QLayout>
#include <QScreen>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <AzQtComponents/Components/Titlebar.h>
#include <AzQtComponents/Components/WindowDecorationWrapper.h>
#include <AzQtComponents/Components/Widgets/LineEdit.h>
#include <cmath>

namespace trickster {
namespace {
class EditorInputAndWindowPolicy final:public QObject {
  public:
    explicit EditorInputAndWindowPolicy(QApplication &app):QObject(&app) {app.installEventFilter(this);}
    bool eventFilter(QObject *object,QEvent *event) override {
        if(event->type()==QEvent::Show)
            if(auto *dialog=qobject_cast<QDialog*>(object);dialog && dialog->testAttribute(Qt::WA_Resized) &&
               !qobject_cast<AzQtComponents::WindowDecorationWrapper*>(dialog->parentWidget()) &&
               !dialog->property("jesterRestoreRequestedSize").toBool()) {
                const auto requested=dialog->size();dialog->setProperty("jesterRestoreRequestedSize",true);
                QTimer::singleShot(0,dialog,[dialog,requested] {
                    auto *frame=qobject_cast<AzQtComponents::WindowDecorationWrapper*>(dialog->parentWidget());
                    if(!frame)return;
                    // Auto-decoration reparents QDialog and Qt can replace an
                    // explicit size with its minimum hint. Restore the intent
                    // after attachment, including the new frame in the bounds.
                    const auto available=frame->screen()->availableGeometry().adjusted(8,8,-8,-8);
                    const auto chrome=frame->size()-dialog->size();
                    dialog->resize(requested.boundedTo(available.size()-chrome));
                    auto anchor=available;
                    if(auto *owner=frame->parentWidget())anchor=owner->window()->frameGeometry();
                    auto position=anchor.center()-QPoint(frame->width()/2,frame->height()/2);
                    position.setX(std::clamp(position.x(),available.left(),std::max(available.left(),available.right()-frame->width()+1)));
                    position.setY(std::clamp(position.y(),available.top(),std::max(available.top(),available.bottom()-frame->height()+1)));
                    frame->move(position);
                });
            }
        if(event->type()==QEvent::Show)
            if(auto *button=qobject_cast<QToolButton*>(object);button && button->objectName()==AzQtComponents::ErrorToolButton)
                if(auto *edit=qobject_cast<QLineEdit*>(button->parentWidget()))
                    QTimer::singleShot(0,button,[button,edit] {
                        // Qt's side action can show its tool button again
                        // after a repolish. Keep the SDK validation state.
                        const bool error=!edit->hasAcceptableInput() || edit->property(AzQtComponents::HasExternalError).toBool();
                        button->setVisible(AzQtComponents::LineEdit::errorIconEnabled(edit) && error);
                    });
        if(event->type()==QEvent::ParentChange || event->type()==QEvent::Show)
            if(auto *window=qobject_cast<QWidget*>(object);window && window->objectName()=="JesterWindow" &&
               qobject_cast<AzQtComponents::WindowDecorationWrapper*>(window->parentWidget())) {
                // The SDK changes guest layouts to SetMinimumSize. The dock
                // workspace already handles compact layouts; preserve its
                // explicit minimum instead of forcing every dock's size hint.
                if(window->layout())window->layout()->setSizeConstraint(QLayout::SetDefaultConstraint);
                window->setMinimumSize(760,420);
            }
        // Let the SDK provide Blue Jay title bars for normal windows/dialogs.
        // Frameless viewport overlays/popups are excluded by the SDK itself.
        if(event->type()==QEvent::Show || event->type()==QEvent::StyleChange)
            if(auto *title=qobject_cast<AzQtComponents::TitleBar*>(object);title && !title->drawSimple())
                QTimer::singleShot(0,title,[title] {
                    if(auto *label=title->findChild<QLabel*>("icon")) {
                        label->setPixmap(editorLogo().pixmap(QSize(24,24),title->devicePixelRatioF()));
                        label->show();
                    }
                });
        if(event->type()==QEvent::ShortcutOverride) {
            auto *focus=QApplication::focusWidget();
            bool editing=false;
            for(auto *widget=focus;widget;widget=widget->parentWidget()) {
                if(qobject_cast<QLineEdit*>(widget) || qobject_cast<QTextEdit*>(widget) ||
                   qobject_cast<QPlainTextEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget) ||
                   qobject_cast<QComboBox*>(widget) || qobject_cast<QKeySequenceEdit*>(widget)) {editing=true;break;}
            }
            if(editing) {
                // Reserve editing keys for the focused field. KeyPress still
                // reaches the widget, including its own clipboard/undo logic.
                const auto *key=static_cast<QKeyEvent*>(event);
                const bool recording=qobject_cast<QKeySequenceEdit*>(focus) || (focus && focus->parentWidget() && qobject_cast<QKeySequenceEdit*>(focus->parentWidget()));
                const bool global=key->modifiers().testFlag(Qt::ControlModifier) &&
                    (key->key()==Qt::Key_S || key->key()==Qt::Key_O || key->key()==Qt::Key_Comma);
                if(recording || !global) {event->accept();return true;}
            }
        }
        return false;
    }
};
}
void applyEditorAppearance(QApplication &app,const EditorPreferences &preferences) {
    app.setAttribute(Qt::AA_DontUseNativeDialogs,true);
    app.setAttribute(Qt::AA_DontUseNativeMenuBar,true);
    if(!app.property("jesterInputAndWindowPolicy").toBool()) {
        new EditorInputAndWindowPolicy(app);app.setProperty("jesterInputAndWindowPolicy",true);
    }
    const auto appearance=preferences.toJson()["appearance"].toObject();
    // Preserve relative heading sizes/weight while updating their family and
    // base size. Explicit local fonts otherwise retain the previous app font.
    const double oldBase=app.font().pointSizeF();
    for(auto *window:app.topLevelWidgets())for(auto *widget:window->findChildren<QWidget*>())
        if(widget->testAttribute(Qt::WA_SetFont) && !widget->property("jesterRelativeFontSize").isValid() && oldBase>0 && widget->font().pointSizeF()>0)
            widget->setProperty("jesterRelativeFontSize",widget->font().pointSizeF()/oldBase);
    const auto theme=appearance["theme"].toString();
    const auto previous=app.property("jesterAppliedAppearance").toJsonObject();
    const auto scale=appearance["uiScale"].toDouble();
    if(previous!=appearance) {
    QList<std::function<void()>> drafts;
    for(auto *window:app.topLevelWidgets())drafts<<preserveUiDrafts(*window);
    applyBlueJayStyle(app,preferences);
    const bool light=theme=="light",contrast=theme=="high-contrast";
    const QColor accent(appearance["accent"].toString());
    const QColor window=contrast?QColor("#000000"):light?QColor("#f2f2f2"):QColor("#444444");
    const QColor base=contrast?QColor("#000000"):light?QColor("#ffffff"):QColor("#222222");
    const QColor text=contrast?QColor("#ffffff"):light?QColor("#222222"):QColor("#cccccc");
    const QColor button=contrast?QColor("#141414"):light?QColor("#e2e2e2"):QColor("#666666");
    QPalette palette;
    palette.setColor(QPalette::Window,window);palette.setColor(QPalette::WindowText,text);
    palette.setColor(QPalette::Base,base);palette.setColor(QPalette::Text,text);
    palette.setColor(QPalette::AlternateBase,light?QColor("#eeeeee"):QColor("#2b2b2b"));
    palette.setColor(QPalette::Button,button);palette.setColor(QPalette::ButtonText,text);
    palette.setColor(QPalette::Highlight,accent);
    auto linear=[](double c){return c<=.04045?c/12.92:std::pow((c+.055)/1.055,2.4);};
    const auto luminance=.2126*linear(accent.redF())+.7152*linear(accent.greenF())+.0722*linear(accent.blueF());
    palette.setColor(QPalette::HighlightedText,luminance>.179?QColor(Qt::black):QColor(Qt::white));
    palette.setColor(QPalette::ToolTipBase,base);palette.setColor(QPalette::ToolTipText,text);
    palette.setColor(QPalette::PlaceholderText,light?QColor("#666666"):QColor("#999999"));
    palette.setColor(QPalette::Link,QColor(BlueJay::Blue));
    palette.setColor(QPalette::Mid,contrast?QColor(Qt::white):light?QColor("#bbbbbb"):QColor("#555555"));
    palette.setColor(QPalette::Dark,contrast?QColor(Qt::white):button.darker(140));
    palette.setColor(QPalette::Light,contrast?QColor(Qt::white):button.lighter(130));
    palette.setColor(QPalette::Disabled,QPalette::Text,QColor("#999999"));
    palette.setColor(QPalette::Disabled,QPalette::ButtonText,palette.color(QPalette::Disabled,QPalette::Text));
    app.setPalette(palette);
    QFont font(appearance["fontFamily"].toString());
    // Font sizes and style metrics remain device-independent; Qt applies the
    // screen DPI once. This scale is not a second devicePixelRatio factor.
    font.setPointSizeF(appearance["fontSize"].toDouble()*scale);app.setFont(font);
    for(const auto &restore:drafts)restore();
    refreshEditorIcons();
    app.setProperty("jesterAppliedAppearance",appearance);
    }
    const int iconSize=int(std::lround(appearance["iconSize"].toInt()*scale));
    for(auto *window:app.topLevelWidgets())
        {
            for(auto *widget:window->findChildren<QWidget*>())if(widget->property("jesterRelativeFontSize").isValid()) {
                auto font=widget->font();font.setFamily(app.font().family());
                font.setPointSizeF(app.font().pointSizeF()*widget->property("jesterRelativeFontSize").toDouble());
                if(widget->font()!=font)widget->setFont(font);
            }
            for(auto *toolbar:window->findChildren<QToolBar*>())toolbar->setIconSize({iconSize,iconSize});
            for(auto *button:window->findChildren<QAbstractButton*>())if(!button->icon().isNull()) {
                button->setIconSize({iconSize,iconSize});
                if(button->property("jesterTransportButton").toBool()) {
                    const int margin=int(std::lround((appearance["density"]=="compact"?8:14)*scale));
                    button->setFixedSize(iconSize+margin,iconSize+margin);
                }
            }
        }
}
QColor overlayColor(const char *name,int alpha) {
    QColor result(PreferencesService::instance().current().value(QString("overlays.")+name).toString());
    result.setAlpha(alpha);return result;
}
} // namespace trickster
