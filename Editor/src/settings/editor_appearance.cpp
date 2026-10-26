#include "settings/editor_appearance.h"
#include "settings/preferences_service.h"
#include "ui/widgets/icons.h"
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QTextEdit>
#include <QToolBar>
#include <QWidget>
#include <cmath>

namespace trickster {
namespace {
class EditorStyle final:public QProxyStyle {
    double scale;
    int iconSize;
    bool compact;
  public:
    explicit EditorStyle(const EditorPreferences &preferences):QProxyStyle(QStyleFactory::create("Fusion")),
        scale(preferences.value("appearance.uiScale").toDouble()),
        iconSize(preferences.value("appearance.iconSize").toInt()),
        compact(preferences.value("appearance.density")=="compact") {}
    int pixelMetric(PixelMetric metric,const QStyleOption *option=nullptr,const QWidget *widget=nullptr) const override {
        switch(metric) {
        case PM_SmallIconSize:case PM_ToolBarIconSize:case PM_ButtonIconSize:return int(std::lround(iconSize*scale));
        case PM_ToolBarItemMargin:return int(std::lround((compact?2:5)*scale));
        case PM_ToolBarItemSpacing:return int(std::lround((compact?3:6)*scale));
        case PM_LayoutHorizontalSpacing:case PM_LayoutVerticalSpacing:return int(std::lround((compact?5:9)*scale));
        case PM_ButtonMargin:return int(std::lround((compact?5:9)*scale));
        default:return QProxyStyle::pixelMetric(metric,option,widget);
        }
    }
};
class EditorInputAndWindowPolicy final:public QObject {
  public:
    explicit EditorInputAndWindowPolicy(QApplication &app):QObject(&app) {app.installEventFilter(this);}
    bool eventFilter(QObject *object,QEvent *event) override {
        if(event->type()==QEvent::Show)
            if(auto *widget=qobject_cast<QWidget*>(object);widget && widget->isWindow())
                widget->setProperty("HasNoWindowDecorations",true);
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
    app.setStyle(new EditorStyle(preferences));
    const bool light=theme=="light",contrast=theme=="high-contrast";
    const QColor accent(appearance["accent"].toString());
    const QColor window=contrast?QColor("#000000"):light?QColor("#f2f1ed"):QColor("#292d35");
    const QColor base=contrast?QColor("#000000"):light?QColor("#ffffff"):QColor("#1f232a");
    const QColor text=contrast?QColor("#ffffff"):light?QColor("#24272e"):QColor("#e6e7eb");
    const QColor button=contrast?QColor("#141414"):light?QColor("#e2e4e8"):QColor("#363b46");
    QPalette palette;
    palette.setColor(QPalette::Window,window);palette.setColor(QPalette::WindowText,text);
    palette.setColor(QPalette::Base,base);palette.setColor(QPalette::Text,text);
    palette.setColor(QPalette::AlternateBase,light?QColor("#eceef1"):QColor("#30343d"));
    palette.setColor(QPalette::Button,button);palette.setColor(QPalette::ButtonText,text);
    palette.setColor(QPalette::Highlight,accent);
    palette.setColor(QPalette::HighlightedText,accent.lightnessF()>.5?QColor(Qt::black):QColor(Qt::white));
    palette.setColor(QPalette::ToolTipBase,base);palette.setColor(QPalette::ToolTipText,text);
    palette.setColor(QPalette::PlaceholderText,light?QColor("#696f79"):QColor("#9ea6b2"));
    palette.setColor(QPalette::Link,light?accent.darker(150):accent.lighter(120));
    palette.setColor(QPalette::Mid,contrast?QColor(Qt::white):light?QColor("#b9bec8"):QColor("#4b515d"));
    palette.setColor(QPalette::Dark,contrast?QColor(Qt::white):button.darker(140));
    palette.setColor(QPalette::Light,contrast?QColor(Qt::white):button.lighter(130));
    palette.setColor(QPalette::Disabled,QPalette::Text,light?QColor("#777c85"):QColor("#8b939f"));
    palette.setColor(QPalette::Disabled,QPalette::ButtonText,palette.color(QPalette::Disabled,QPalette::Text));
    app.setPalette(palette);
    QFont font(appearance["fontFamily"].toString());
    // Font sizes and style metrics remain device-independent; Qt applies the
    // screen DPI once. This scale is not a second devicePixelRatio factor.
    font.setPointSizeF(appearance["fontSize"].toDouble()*scale);app.setFont(font);
    app.setStyleSheet("QToolBar { border: 0; } QToolButton { border-radius: 3px; } "
        "QToolButton:hover { background: palette(mid); } QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); } "
        "QDockWidget::title { padding: 4px 6px; background: palette(button); } "
        "QGroupBox { border: 1px solid palette(mid); border-radius: 3px; margin-top: 10px; padding-top: 7px; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 7px; padding: 0 3px; } "
        "QToolTip { border: 1px solid palette(mid); padding: 3px; }");
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
