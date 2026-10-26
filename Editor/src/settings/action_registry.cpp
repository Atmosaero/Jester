#include "settings/action_registry.h"
#include <QAction>
#include <QMenu>
#include <QDockWidget>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSet>
#include <QWidget>
#include <QLineEdit>
namespace trickster {
namespace {
QList<QKeySequence> effective(QAction *action,const EditorPreferences &preferences) {
    const auto overrides=preferences.toJson()["shortcuts"].toObject();
    const auto key=ActionRegistry::id(action);
    if(overrides.contains(key)) {
        const auto text=overrides[key].toString();
        return text.isEmpty()?QList<QKeySequence>{}:QList<QKeySequence>{QKeySequence::fromString(text,QKeySequence::PortableText)};
    }
    QList<QKeySequence> defaults;
    for(const auto &text:action->property("jesterDefaultShortcuts").toStringList())defaults.push_back(QKeySequence::fromString(text,QKeySequence::PortableText));
    return defaults;
}
}
QString ActionRegistry::id(const QAction *action) {return action->property("tricksterActionId").toString();}
ActionRegistry::ActionRegistry(QWidget &root) {
    for(auto *dock:root.findChildren<QDockWidget*>()) {
        auto *action=dock->toggleViewAction();
        if(action->objectName().isEmpty())action->setObjectName("toggle_"+dock->objectName());
    }
    QSet<QString> used;
    for(auto *action:root.findChildren<QAction*>()) {
        // SDK search/error/clear icons are implementation actions of a text
        // field. They are created lazily during polish, not editor commands.
        if(action->isSeparator() || action->menu() || qobject_cast<QLineEdit*>(action->parent()))continue;
        auto key=id(action);
        if(key.isEmpty()) {
            key=action->objectName();
            if(key.isEmpty()) {
                const auto source=action->property("jesterSourceText");
                key=(source.isValid()?source.toString():action->text()).toLower();key.remove('&');
                key.replace(QRegularExpression("[^a-z0-9]+"),"_");key.remove(QRegularExpression("^_|_$"));
            }
            if(key.isEmpty())key="action";
            const auto base=key;int suffix=2;while(used.contains(key))key=base+"_"+QString::number(suffix++);
            action->setProperty("tricksterActionId",key);
        }
        used.insert(key);
        if(!action->property("jesterDefaultShortcuts").isValid()) {
            QStringList defaults;for(const auto &shortcut:action->shortcuts())defaults<<shortcut.toString(QKeySequence::PortableText);
            action->setProperty("jesterDefaultShortcuts",defaults);
        }
        registered.push_back(action);
    }
}
bool ActionRegistry::validate(const EditorPreferences &preferences,QString &error) const {
    struct Binding {QString id;QKeySequence sequence;};QList<Binding> bindings;
    QSet<QString> known;for(auto *action:registered)known.insert(id(action));
    const auto overrides=preferences.toJson()["shortcuts"].toObject();
    for(auto it=overrides.begin();it!=overrides.end();++it)if(!known.contains(it.key())) {
        error="preferences.unknown_action: "+it.key();return false;
    }
    for(auto *action:registered)for(const auto &sequence:effective(action,preferences)) {
        if(sequence.isEmpty())continue;
        for(const auto &other:bindings)if(other.id!=id(action) &&
            (sequence.matches(other.sequence)!=QKeySequence::NoMatch || other.sequence.matches(sequence)!=QKeySequence::NoMatch)) {
            error="preferences.shortcut_conflict: "+id(action)+" / "+other.id+" ("+sequence.toString(QKeySequence::PortableText)+")";return false;
        }
        bindings.push_back({id(action),sequence});
    }
    error.clear();return true;
}
void ActionRegistry::apply(const EditorPreferences &preferences) const {
    for(auto *action:registered)action->setShortcuts(effective(action,preferences));
}
QJsonArray ActionRegistry::state() const {
    QJsonArray result;
    for(auto *action:registered) {
        QJsonArray bindings;for(const auto &key:action->shortcuts())bindings.append(key.toString(QKeySequence::PortableText));
        result.append(QJsonObject{{"id",id(action)},{"text",action->text()},{"shortcuts",bindings}});
    }
    return result;
}
}
