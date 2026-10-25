#include "core/scene/runtime_error.h"
#include "automation/editor_api.h"
#include "core/scene/json_helpers.h"
#include "ui/window/window.h"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QSlider>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <functional>
namespace trickster {
using namespace api_detail;
void EditorApi::cancelInput() {
    for (QWidget *target : {static_cast<QWidget *>(w->viewport), static_cast<QWidget *>(w->timeline)}) {
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(target, &escape);
        target->setProperty("tricksterPointerButton", 0);
    }
}
void EditorApi::nameActions() {
    QSet<QString> ids;
    for (auto a : w->findChildren<QAction *>()) {
        if (a->isSeparator() || a->menu())
            continue;
        auto id = a->objectName();
        if (id.isEmpty()) {
            id = a->text().toLower();
            id.remove('&');
            id.replace(QRegularExpression("[^a-z0-9]+"), "_");
            id.remove(QRegularExpression("^_|_$"));
        }
        if (id.isEmpty())
            id = "action";
        auto base = id;
        int suffix = 2;
        while (ids.contains(id))
            id = base + "_" + QString::number(suffix++);
        ids.insert(id);
        a->setProperty("tricksterActionId", id);
    }
}
QJsonArray EditorApi::actions() const {
    QJsonArray result;
    for (auto a : w->findChildren<QAction *>()) {
        auto id = a->property("tricksterActionId").toString();
        if (id.isEmpty())
            continue;
        QJsonArray shortcuts;
        for (auto key : a->shortcuts())
            shortcuts.append(key.toString(QKeySequence::PortableText));
        result.append(QJsonObject{{"id", id},
                                  {"objectName", a->objectName()},
                                  {"text", a->text()},
                                  {"enabled", a->isEnabled()},
                                  {"visible", a->isVisible()},
                                  {"checkable", a->isCheckable()},
                                  {"checked", a->isChecked()},
                                  {"shortcuts", shortcuts},
                                  {"tooltip", a->toolTip()}});
    }
    return result;
}
QString EditorApi::uiId(QObject *object) const {
    if (!object)
        return {};
    auto id = object->property("tricksterUiId").toString();
    if (id.isEmpty()) {
        id = "ui_" + QString::number(nextUiId++);
        object->setProperty("tricksterUiId", id);
    }
    return id;
}
QObject *EditorApi::findUi(const QString &id) const {
    QList<QObject *> objects = w->findChildren<QObject *>();
    objects.prepend(w);
    QObject *found = nullptr;
    for (auto object : objects)
        if (uiId(object) == id || (!object->objectName().isEmpty() && object->objectName() == id)) {
            require(!found, "Ambiguous objectName; use the ui ID from ui_inspect");
            found = object;
        }
    require(found != nullptr, "Control no longer exists or was not found: " + id);
    return found;
}
QJsonArray EditorApi::controls() const {
    QJsonArray result;
    auto widgets = w->findChildren<QWidget *>();
    widgets.prepend(w);
    for (auto widget : widgets) {
        bool interactive = qobject_cast<QAbstractButton *>(widget) ||
                           qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QLineEdit *>(widget) ||
                           qobject_cast<QComboBox *>(widget) || qobject_cast<QAbstractItemView *>(widget) ||
                           qobject_cast<QSlider *>(widget) || qobject_cast<QTabBar *>(widget) ||
                           qobject_cast<QTabWidget *>(widget) || qobject_cast<QDialog *>(widget) ||
                           qobject_cast<QMenu *>(widget) || widget == w->viewport || widget == w->timeline;
        if (!interactive && widget->objectName().isEmpty())
            continue;
        auto pos = w->mapFromGlobal(widget->mapToGlobal(QPoint()));
        QJsonObject o{{"id", uiId(widget)},
                      {"objectName", widget->objectName()},
                      {"class", widget->metaObject()->className()},
                      {"parent", uiId(widget->parentWidget())},
                      {"visible", widget->isVisible()},
                      {"enabled", widget->isEnabled()},
                      {"x", pos.x()},
                      {"y", pos.y()},
                      {"width", widget->width()},
                      {"height", widget->height()},
                      {"tooltip", widget->toolTip()},
                      {"accessibleName", widget->accessibleName()}};
        if (widget->isWindow())
            o["title"] = widget->windowTitle();
        if (auto text = qobject_cast<QPlainTextEdit *>(widget))
            o["text"] = text->toPlainText();
        if (auto text = qobject_cast<QTextEdit *>(widget))
            o["text"] = text->toPlainText();
        if (auto menu = qobject_cast<QMenu *>(widget)) {
            QJsonArray entries;
            for (auto action : menu->actions())
                entries.append(QJsonObject{{"id", action->property("tricksterActionId").toString()},
                                           {"text", action->text()},
                                           {"enabled", action->isEnabled()}});
            o["actions"] = entries;
        }
        if (auto b = qobject_cast<QAbstractButton *>(widget)) {
            o["text"] = b->text();
            o["checkable"] = b->isCheckable();
            o["checked"] = b->isChecked();
        }
        if (auto b = qobject_cast<QSpinBox *>(widget)) {
            o["value"] = b->value();
            o["minimum"] = b->minimum();
            o["maximum"] = b->maximum();
            o["readOnly"] = b->isReadOnly();
        }
        if (auto b = qobject_cast<QDoubleSpinBox *>(widget)) {
            o["value"] = b->value();
            o["minimum"] = b->minimum();
            o["maximum"] = b->maximum();
            o["readOnly"] = b->isReadOnly();
        }
        if (auto b = qobject_cast<QLineEdit *>(widget)) {
            o["value"] = b->echoMode() == QLineEdit::Normal ? b->text() : QString("<hidden>");
            o["readOnly"] = b->isReadOnly();
        }
        if (auto b = qobject_cast<QSlider *>(widget)) {
            o["value"] = b->value();
            o["minimum"] = b->minimum();
            o["maximum"] = b->maximum();
        }
        if (auto b = qobject_cast<QComboBox *>(widget)) {
            QJsonArray items;
            for (int i = 0; i < b->count(); ++i)
                items.append(b->itemText(i));
            o["items"] = items;
            o["index"] = b->currentIndex();
        }
        if (auto b = qobject_cast<QTabBar *>(widget)) {
            QJsonArray items;
            for (int i = 0; i < b->count(); ++i)
                items.append(b->tabText(i));
            o["items"] = items;
            o["index"] = b->currentIndex();
        }
        if (auto b = qobject_cast<QTabWidget *>(widget)) {
            QJsonArray items;
            for (int i = 0; i < b->count(); ++i)
                items.append(b->tabText(i));
            o["items"] = items;
            o["index"] = b->currentIndex();
        }
        if (auto b = qobject_cast<QLabel *>(widget))
            o["text"] = b->text();
        if (auto view = qobject_cast<QAbstractItemView *>(widget)) {
            QJsonArray items;
            int count = 0;
            std::function<void(QModelIndex, QJsonArray, int)> walk = [&](QModelIndex parent, QJsonArray path,
                                                                         int depth) {
                if (!view->model() || depth > 24)
                    return;
                int rows = view->model()->rowCount(parent);
                for (int row = 0; row < rows && count < 4096; ++row) {
                    auto index = view->model()->index(row, 0, parent);
                    auto child = path;
                    child.append(row);
                    ++count;
                    QJsonObject item{
                        {"path", child},
                        {"text", index.data().toString()},
                        {"selected", view->selectionModel() && view->selectionModel()->isSelected(index)}};
                    auto data = index.data(Qt::UserRole);
                    if (data.isValid())
                        item["data"] = QJsonValue::fromVariant(data);
                    items.append(item);
                    walk(index, child, depth + 1);
                }
            };
            walk(view->rootIndex(), {}, 0);
            o["items"] = items;
            o["truncated"] = count >= 4096;
        }
        result.append(o);
    }
    return result;
}
QJsonObject EditorApi::uiCall(const QString &name, const QJsonObject &a) {
    if (name == "ui_inspect") {
        QJsonArray items;
        if (a.contains("id")) {
            auto object = findUi(a["id"].toString());
            auto id = uiId(object);
            for (auto v : controls())
                if (v.toObject()["id"] == id)
                    items.append(v);
            require(!items.empty(), "Object is not an inspectable widget");
        } else
            for (auto v : controls())
                if (!a["visibleOnly"].toBool(true) || v.toObject()["visible"].toBool())
                    items.append(v);
        return {{"controls", items},
                {"modal",
                 QApplication::activeModalWidget() ? uiId(QApplication::activeModalWidget()) : QString()}};
    }
    if (name == "editor_action") {
        QAction *action = nullptr;
        auto id = a["id"].toString();
        for (auto candidate : w->findChildren<QAction *>())
            if (candidate->property("tricksterActionId") == id ||
                (!candidate->objectName().isEmpty() && candidate->objectName() == id)) {
                require(!action, "Ambiguous action ID");
                action = candidate;
            }
        require(action != nullptr, "Action not found: " + id);
        require(action->isEnabled(), "Action is disabled");
        if (a.contains("checked")) {
            require(action->isCheckable(), "Action is not checkable");
            if (action->isChecked() == a["checked"].toBool())
                return {{"queued", false}, {"unchanged", true}};
        }
        QPointer<QAction> guarded = action;
        QTimer::singleShot(0, this, [guarded] {
            if (guarded && guarded->isEnabled())
                guarded->trigger();
        });
        return {{"queued", true}, {"id", id}};
    }
    auto object = findUi(a["id"].toString());
    auto widget = qobject_cast<QWidget *>(object);
    require(widget, "Object is not a widget");
    require(widget->isEnabled(), "Control is disabled");
    if (auto modal = QApplication::activeModalWidget())
        require(widget == modal || modal->isAncestorOf(widget), "A modal dialog blocks this control");
    if (name == "ui_set") {
        auto value = a["value"];
        if (auto spin = qobject_cast<QDoubleSpinBox *>(widget)) {
            require(value.isDouble() && value.toDouble() >= spin->minimum() &&
                        value.toDouble() <= spin->maximum(),
                    "Value outside spinbox range");
            require(!spin->isReadOnly(), "Control is read-only");
            spin->setValue(value.toDouble());
            QMetaObject::invokeMethod(spin, "editingFinished", Qt::DirectConnection);
        } else if (auto integer = qobject_cast<QSpinBox *>(widget)) {
            require(value.isDouble() && std::floor(value.toDouble()) == value.toDouble() &&
                        value.toDouble() >= integer->minimum() && value.toDouble() <= integer->maximum(),
                    "Value outside integer spinbox range");
            require(!integer->isReadOnly(), "Control is read-only");
            integer->setValue(value.toInt());
            QMetaObject::invokeMethod(integer, "editingFinished", Qt::DirectConnection);
        } else if (auto button = qobject_cast<QAbstractButton *>(widget)) {
            require(button->isCheckable() && value.isBool(), "Expected a checkable control and boolean");
            button->setChecked(value.toBool());
        } else if (auto slider = qobject_cast<QSlider *>(widget)) {
            require(value.isDouble() && value.toDouble() == std::floor(value.toDouble()) &&
                        value.toDouble() >= slider->minimum() && value.toDouble() <= slider->maximum(),
                    "Value outside slider range");
            slider->setValue(value.toInt());
        } else if (auto edit = qobject_cast<QLineEdit *>(widget)) {
            require(value.isString() && !edit->isReadOnly(), "Expected an editable text field");
            edit->setText(value.toString());
            QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection);
        } else if (auto edit = qobject_cast<QTextEdit *>(widget)) {
            require(value.isString() && !edit->isReadOnly(), "Expected an editable text field");
            edit->setPlainText(value.toString());edit->document()->setModified(true);
        } else if (auto edit = qobject_cast<QPlainTextEdit *>(widget)) {
            require(value.isString() && !edit->isReadOnly(), "Expected an editable text field");
            edit->setPlainText(value.toString());edit->document()->setModified(true);
        } else if (auto combo = qobject_cast<QComboBox *>(widget)) {
            int i = value.isString() ? combo->findText(value.toString()) : value.toInt(-1);
            require(i >= 0 && i < combo->count(), "Combo item not found");
            combo->setCurrentIndex(i);
        } else
            throw trickster::RuntimeError("Control does not support ui_set");
        return {{"updated", true}};
    }
    if (name == "ui_click") {
        auto op = a["operation"].toString("click");
        QPointer<QWidget> guarded = widget;
        if (op == "accept" || op == "reject")
            require(qobject_cast<QDialog *>(widget), "Accept/reject require a dialog");
        else
            require(qobject_cast<QAbstractButton *>(widget), "Click requires a button");
        QTimer::singleShot(0, this, [guarded, op] {
            if (!guarded)
                return;
            if (auto dialog = qobject_cast<QDialog *>(guarded.data())) {
                QMetaObject::invokeMethod(dialog, op.toUtf8().constData(), Qt::DirectConnection);
                return;
            }
            if (auto tool = qobject_cast<QToolButton *>(guarded.data()))
                if (tool->menu() && tool->popupMode() == QToolButton::InstantPopup) {
                    tool->showMenu();
                    return;
                }
            qobject_cast<QAbstractButton *>(guarded.data())->click();
        });
        return {{"queued", true}};
    }
    if (name == "ui_select") {
        if (auto view = qobject_cast<QAbstractItemView *>(widget)) {
            auto path = a["path"].toArray();
            if (path.empty()) {
                required(a, "index");
                path.append(a["index"]);
            }
            require(view->model() != nullptr, "View has no model");
            auto index = view->rootIndex();
            for (auto row : path) {
                require(row.toInt() >= 0 && row.toInt() < view->model()->rowCount(index),
                        "Item path outside model");
                index = view->model()->index(row.toInt(), 0, index);
            }
            require(index.isValid(), "Invalid model index");
            view->setCurrentIndex(index);
            view->scrollTo(index);
        } else {
            required(a, "index");
            int i = a["index"].toInt();
            if (auto tabs = qobject_cast<QTabBar *>(widget)) {
                require(i < tabs->count(), "Tab index out of range");
                tabs->setCurrentIndex(i);
            } else if (auto tabWidget = qobject_cast<QTabWidget *>(widget)) {
                require(i < tabWidget->count(), "Tab index out of range");
                tabWidget->setCurrentIndex(i);
            } else if (auto combo = qobject_cast<QComboBox *>(widget)) {
                require(i < combo->count(), "Combo index out of range");
                combo->setCurrentIndex(i);
            } else
                throw trickster::RuntimeError("Control does not support selection");
        }
        return {{"selected", true}};
    }
    if (name == "ui_key") {
        auto sequence = QKeySequence::fromString(a["sequence"].toString(), QKeySequence::PortableText);
        require(sequence.count() == 1 && !sequence.isEmpty(), "Use a single key combination");
        auto combination = sequence[0];
        if (widget == w || widget == w->viewport) {
            for (auto action : w->findChildren<QAction *>())
                if (action->isEnabled() && action->shortcuts().contains(sequence)) {
                    QPointer<QAction> guarded = action;
                    QTimer::singleShot(0, this, [guarded] {
                        if (guarded)
                            guarded->trigger();
                    });
                    return {{"queued", true}};
                }
        }
        if (combination.key() == Qt::Key_Escape)
            widget->setProperty("tricksterPointerButton", 0);
        widget->setFocus();
        QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(),
                        combination.keyboardModifiers() == Qt::NoModifier ? sequence.toString() : QString());
        QApplication::sendEvent(widget, &press);
        QKeyEvent release(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers());
        QApplication::sendEvent(widget, &release);
        return {{"sent", true}};
    }
    if (name == "ui_pointer") {
        require(widget == w->viewport || widget == w->timeline || qobject_cast<QAbstractItemView *>(widget),
                "Pointer gestures target a viewport, timeline or item view");
        auto op = a["operation"].toString();
        required(a, "x");
        required(a, "y");
        if (op == "drag") {
            required(a, "toX");
            required(a, "toY");
        }
        if (op == "wheel")
            required(a, "delta");
        QPointF from(a["x"].toDouble(), a["y"].toDouble());
        auto button = a["button"].toString("left");
        Qt::MouseButton b = button == "right"    ? Qt::RightButton
                            : button == "middle" ? Qt::MiddleButton
                                                 : Qt::LeftButton;
        Qt::KeyboardModifiers modifiers;
        for (auto v : a["modifiers"].toArray()) {
            if (v == "ctrl")
                modifiers |= Qt::ControlModifier;
            if (v == "shift")
                modifiers |= Qt::ShiftModifier;
            if (v == "alt")
                modifiers |= Qt::AltModifier;
        }
        auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButton changed, Qt::MouseButtons held) {
            QMouseEvent event(type, point, widget->mapToGlobal(point), changed, held, modifiers);
            QApplication::sendEvent(widget, &event);
        };
        if (op == "wheel") {
            QWheelEvent event(from, widget->mapToGlobal(from), {}, QPoint(0, a["delta"].toInt()),
                              Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
            QApplication::sendEvent(widget, &event);
        } else if (op == "update") {
            auto held = Qt::MouseButton(widget->property("tricksterPointerButton").toInt());
            require(held != Qt::NoButton, "No pointer gesture started");
            send(QEvent::MouseMove, from, Qt::NoButton, held);
        } else if (op == "end") {
            auto held = Qt::MouseButton(widget->property("tricksterPointerButton").toInt());
            require(held != Qt::NoButton, "No pointer gesture started");
            send(QEvent::MouseButtonRelease, from, held, Qt::NoButton);
            widget->setProperty("tricksterPointerButton", 0);
        } else {
            send(op == "double_click" ? QEvent::MouseButtonDblClick : QEvent::MouseButtonPress, from, b, b);
            if (op == "begin")
                widget->setProperty("tricksterPointerButton", int(b));
            else {
                QPointF to = from;
                if (op == "drag") {
                    required(a, "toX");
                    required(a, "toY");
                    to = {a["toX"].toDouble(), a["toY"].toDouble()};
                    for (int i = 1; i <= 16; ++i)
                        send(QEvent::MouseMove, from + (to - from) * (double(i) / 16), Qt::NoButton, b);
                }
                send(QEvent::MouseButtonRelease, to, b, Qt::NoButton);
            }
        }
        return {{"sent", true}, {"dragging", w->viewport->isDragging()}};
    }
    throw trickster::RuntimeError("Unknown UI operation");
}
} // namespace trickster
