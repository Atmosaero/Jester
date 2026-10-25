#pragma once
#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QWidget>
#include <QEvent>
namespace trickster {
class JesterNativeWindows final : public QObject {
public:
    explicit JesterNativeWindows(QApplication& app) : QObject(&app) { app.installEventFilter(this); }
protected:
    bool eventFilter(QObject* object,QEvent* event) override {
        if(event->type()==QEvent::Show)
            if(auto* widget=qobject_cast<QWidget*>(object);widget && widget->isWindow())
                widget->setProperty("HasNoWindowDecorations",true);
        return false;
    }
};
inline void applyAppearance(QApplication &app) {
    if(!app.property("jesterNativeWindows").toBool()) {
        new JesterNativeWindows(app);
        app.setProperty("jesterNativeWindows",true);
    }
    app.setStyleSheet(QString());
    app.setStyle("Fusion");
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(42, 47, 56));
    palette.setColor(QPalette::WindowText, QColor(225, 231, 239));
    palette.setColor(QPalette::Base, QColor(32, 37, 45));
    palette.setColor(QPalette::AlternateBase, QColor(40, 46, 56));
    palette.setColor(QPalette::Text, QColor(225, 231, 239));
    palette.setColor(QPalette::PlaceholderText, QColor(145, 159, 176));
    palette.setColor(QPalette::Button, QColor(54, 62, 74));
    palette.setColor(QPalette::ButtonText, QColor(225, 231, 239));
    palette.setColor(QPalette::Highlight, QColor(54, 119, 154));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, QColor(48, 55, 65));
    palette.setColor(QPalette::ToolTipText, Qt::white);
    palette.setColor(QPalette::Link, QColor(104, 196, 234));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(126, 139, 154));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(126, 139, 154));
    app.setPalette(palette);
    QFont font("Segoe UI", 10);
    app.setFont(font);
}
} // namespace trickster
