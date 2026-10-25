#include "settings/preferences.h"
#include "core/scene/visual_settings.h"
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace trickster {
namespace {
QJsonObject defaults() {
    return {{"format","JesterPreferences"},{"version",1},{"language","system"},
        {"appearance",QJsonObject{{"fontFamily","Segoe UI"},{"fontSize",10.0},{"uiScale",1.0},
            {"iconSize",18},{"density","compact"},{"theme","dark"},{"accent","#d7a64f"}}},
        {"overlays",QJsonObject{{"bones","#8ea9bc"},{"selection","#efbb62"},{"notes","#ecd58a"},
                                {"trajectory","#62bdb4"},{"manipulatorX","#df7168"},
                                {"manipulatorY","#72c58a"},{"manipulatorZ","#7ba5e2"}}},
        {"shortcuts",QJsonObject{}},{"layouts",QJsonObject{}},{"scenePresets",QJsonObject{}}};
}
bool color(const QJsonValue &value) {
    return value.isString() && QRegularExpression("^#[0-9a-fA-F]{6}$").match(value.toString()).hasMatch() && QColor(value.toString()).isValid();
}
bool number(const QJsonValue &value,double low,double high) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble()>=low && value.toDouble()<=high;
}
bool choice(const QJsonValue &value,const QStringList &choices) {
    return value.isString() && choices.contains(value.toString());
}
bool merge(QJsonObject &target,const QJsonObject &patch,QString &error,const QString &prefix={}) {
    for(auto it=patch.begin();it!=patch.end();++it) {
        const auto name=prefix+it.key();
        if(!target.contains(it.key())) {error="preferences.unknown_parameter: "+name;return false;}
        if(it.key()=="shortcuts" || it.key()=="layouts" || it.key()=="scenePresets") {
            if(!it->isObject()) {error="preferences.expected_object: "+name;return false;}
            target[it.key()]=it.value();
        } else if(target[it.key()].isObject()) {
            if(!it->isObject()) {error="preferences.expected_object: "+name;return false;}
            auto child=target[it.key()].toObject();
            if(!merge(child,it->toObject(),error,name+"."))return false;
            target[it.key()]=child;
        } else target[it.key()]=it.value();
    }
    return true;
}
}
EditorPreferences::EditorPreferences():values(defaults()) {}
QJsonValue EditorPreferences::value(const QString &path) const {
    QJsonValue result(values);
    for(const auto &part:path.split('.'))result=result.toObject().value(part);
    return result;
}
bool EditorPreferences::patch(const QJsonObject &patch,QString &error) {
    auto candidate=values;
    if(!merge(candidate,patch,error))return false;
    if(candidate["format"]!="JesterPreferences" || !candidate["version"].isDouble() || candidate["version"].toDouble()!=1) {
        error="preferences.unsupported_format_or_version";return false;
    }
    if(!choice(candidate["language"],{"system","en","ru"})) {error="preferences.invalid_language";return false;}
    auto appearance=candidate["appearance"].toObject();
    if(!appearance["fontFamily"].isString() || appearance["fontFamily"].toString().trimmed().isEmpty() ||
       appearance["fontFamily"].toString().size()>128 || !number(appearance["fontSize"],7,32) ||
       !number(appearance["uiScale"],.75,2.5) || !number(appearance["iconSize"],12,48) ||
       appearance["iconSize"].toDouble()!=appearance["iconSize"].toInt() ||
       !choice(appearance["density"],{"compact","comfortable"}) ||
       !choice(appearance["theme"],{"dark","light","high-contrast"}) || !color(appearance["accent"])) {
        error="preferences.invalid_appearance";return false;
    }
    const auto overlays=candidate["overlays"].toObject();
    for(auto it=overlays.begin();it!=overlays.end();++it)if(!color(it.value())) {
        error="preferences.invalid_overlay_color: "+it.key();return false;
    }
    const auto shortcuts=candidate["shortcuts"].toObject();
    if(shortcuts.size()>1000) {error="preferences.too_many_shortcuts";return false;}
    QSet<QString> used;
    for(auto it=shortcuts.begin();it!=shortcuts.end();++it) {
        if(!QRegularExpression("^[a-zA-Z0-9_][a-zA-Z0-9_.-]{0,127}$").match(it.key()).hasMatch() || !it->isString() || it->toString().size()>128) {
            error="preferences.invalid_shortcut: "+it.key();return false;
        }
        const auto text=it->toString();
        if(text.isEmpty())continue; // An explicit empty shortcut disables the default.
        const auto sequence=QKeySequence::fromString(text,QKeySequence::PortableText);
        const auto canonical=sequence.toString(QKeySequence::PortableText);
        if(sequence.isEmpty() || canonical.isEmpty() || canonical.contains("Unknown",Qt::CaseInsensitive)) {
            error="preferences.invalid_shortcut: "+it.key();return false;
        }
        if(used.contains(canonical)) {error="preferences.shortcut_conflict: "+canonical;return false;}
        used.insert(canonical);
    }
    const auto layouts=candidate["layouts"].toObject();
    if(layouts.size()>32) {error="preferences.too_many_layouts";return false;}
    for(auto it=layouts.begin();it!=layouts.end();++it) {
        if(it.key().trimmed().isEmpty() || it.key().size()>80 || !it->isString() || it->toString().size()>131072) {
            error="preferences.invalid_layout: "+it.key();return false;
        }
        auto decoded=QByteArray::fromBase64Encoding(it->toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
        if(!decoded || decoded.decoded.size()<8 || decoded.decoded.size()>65536 ||
           !decoded.decoded.startsWith(QByteArray::fromHex("000000ff"))) {
            error="preferences.invalid_layout: "+it.key();return false;
        }
    }
    const auto presets=candidate["scenePresets"].toObject();
    if(presets.size()>32) {error="preferences.too_many_scene_presets";return false;}
    for(auto it=presets.begin();it!=presets.end();++it) {
        SceneVisualSettings settings;
        if(it.key().trimmed().isEmpty() || it.key().size()>80 || !it->isObject() || !SceneVisualSettings::fromJson(it->toObject(),settings,error)) {
            error="preferences.invalid_scene_preset: "+it.key()+" "+error;return false;
        }
    }
    values=candidate;error.clear();return true;
}
bool EditorPreferences::fromJson(const QJsonObject &json,EditorPreferences &result,QString &error) {
    if(json["format"]!="JesterPreferences" || !json["version"].isDouble() || json["version"].toDouble()!=1) {
        error="preferences.unsupported_format_or_version";return false;
    }
    EditorPreferences candidate;
    if(!candidate.patch(json,error))return false;
    result=std::move(candidate);return true;
}
bool EditorPreferences::load(const QString &path,EditorPreferences &result,QString &error) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) {error="preferences.read_failed: "+file.errorString();return false;}
    if(file.size()>1048576) {error="preferences.file_too_large";return false;}
    QJsonParseError parse;
    auto json=QJsonDocument::fromJson(file.readAll(),&parse);
    if(parse.error!=QJsonParseError::NoError || !json.isObject()) {error="preferences.invalid_json";return false;}
    return fromJson(json.object(),result,error);
}
bool EditorPreferences::save(const QString &path,QString &error) const {
    QSaveFile file(path);
    const auto bytes=QJsonDocument(values).toJson(QJsonDocument::Indented);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {
        error="preferences.write_failed: "+file.errorString();return false;
    }
    error.clear();return true;
}
} // namespace trickster
