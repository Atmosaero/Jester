#include "core/scene/visual_settings.h"
#include <QColor>
#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>
#include <functional>

namespace trickster {
namespace {
QJsonObject light(bool enabled, double azimuth, double elevation, double lux, bool shadows) {
    return {{"enabled",enabled},{"azimuth",azimuth},{"elevation",elevation},
            {"intensityLux",lux},{"color","#ffffff"},{"shadows",shadows},{"shadowQuality","medium"}};
}
QJsonObject defaults() {
    return {{"version",1},{"mode","lit"},{"silhouette",false},
        {"wireframe",QJsonObject{{"color","#eab45b"},{"hiddenEdges",false}}},
        {"lighting",QJsonObject{{"main",light(true,45,50,1600,true)},
                                {"fill",light(true,-70,25,400,false)}}},
        {"environment",QJsonObject{{"enabled",true},{"preset","neutral-studio"},{"exposureEV",0.0},{"rotation",0.0}}},
        {"background",QJsonObject{{"mode","solid"},{"topColor","#343840"},{"bottomColor","#22262d"}}},
        {"floor",QJsonObject{{"enabled",true},{"color","#757575"},{"height",0.0},{"followPhysicsGround",false}}},
        {"grid",QJsonObject{{"enabled",true},{"step",.1},{"intensity",.35},{"axes",true}}},
        {"camera",QJsonObject{{"projection","perspective"},{"fovDegrees",40.0},{"orthographicHeight",2.5}}},
        {"post",QJsonObject{{"enabled",true},{"exposureEV",0.0},{"toneMapper","aces"},
                            {"saturation",1.0},{"contrast",1.0},
                            {"bloom",QJsonObject{{"enabled",false},{"threshold",1.0},{"intensity",.1}}}}}};
}
QJsonValue at(const QJsonObject &object, const QStringList &path) {
    QJsonValue value(object);
    for (const auto &part:path) value=value.toObject().value(part);
    return value;
}
void put(QJsonObject &object, const QStringList &path, const QJsonValue &value, int depth=0) {
    if (depth==path.size()-1) { object[path[depth]]=value; return; }
    auto child=object[path[depth]].toObject();
    put(child,path,value,depth+1);object[path[depth]]=child;
}
bool merge(QJsonObject &target,const QJsonObject &patch,QString &error,const QString &prefix={}) {
    for(auto it=patch.begin();it!=patch.end();++it) {
        auto name=prefix+it.key();
        if(!target.contains(it.key())) { error="visual.unknown_parameter: "+name;return false; }
        if(target[it.key()].isObject()) {
            if(!it->isObject()) { error="visual.expected_object: "+name;return false; }
            auto child=target[it.key()].toObject();
            if(!merge(child,it->toObject(),error,name+"."))return false;
            target[it.key()]=child;
        } else target[it.key()]=it.value();
    }
    return true;
}
}
SceneVisualSettings::SceneVisualSettings():values(defaults()) {}
QJsonValue SceneVisualSettings::value(const QString &path) const { return at(values,path.split('.')); }

const QVector<VisualField> &SceneVisualSettings::fields() {
    static const QVector<VisualField> result=[] {
        QVector<VisualField> f;
        auto add=[&](QString path,const char *group,const char *label,VisualFieldKind kind,double low=0,double high=1,double step=.1,QStringList choices={}) {
            f.push_back({std::move(path),group,label,kind,low,high,step,std::move(choices)});
        };
        using K=VisualFieldKind;
        add("mode","Viewport",QT_TRANSLATE_NOOP("SceneSettings","Display mode"),K::Choice,0,1,1,{"lit","unlit","wireframe"});
        add("silhouette","Viewport",QT_TRANSLATE_NOOP("SceneSettings","Silhouette"),K::Toggle);
        add("wireframe.color","Viewport",QT_TRANSLATE_NOOP("SceneSettings","Wire color"),K::Color);
        add("wireframe.hiddenEdges","Viewport",QT_TRANSLATE_NOOP("SceneSettings","Show hidden edges"),K::Toggle);
        for(const auto &name:{QString("main"),QString("fill")}) {
            const char *group=name=="main"?QT_TRANSLATE_NOOP("SceneSettings","Main light"):QT_TRANSLATE_NOOP("SceneSettings","Fill light");
            auto p="lighting."+name+".";
            add(p+"enabled",group,QT_TRANSLATE_NOOP("SceneSettings","Enabled"),K::Toggle);
            add(p+"azimuth",group,QT_TRANSLATE_NOOP("SceneSettings","Azimuth (degrees)"),K::Number,-180,180,1);
            add(p+"elevation",group,QT_TRANSLATE_NOOP("SceneSettings","Elevation (degrees)"),K::Number,-89,89,1);
            add(p+"intensityLux",group,QT_TRANSLATE_NOOP("SceneSettings","Illuminance (lux)"),K::Number,0,100000,50);
            add(p+"color",group,QT_TRANSLATE_NOOP("SceneSettings","Color"),K::Color);
            add(p+"shadows",group,QT_TRANSLATE_NOOP("SceneSettings","Shadows"),K::Toggle);
            add(p+"shadowQuality",group,QT_TRANSLATE_NOOP("SceneSettings","Shadow quality"),K::Choice,0,1,1,{"low","medium","high"});
        }
        add("environment.enabled","Environment",QT_TRANSLATE_NOOP("SceneSettings","Environment lighting"),K::Toggle);
        add("environment.preset","Environment",QT_TRANSLATE_NOOP("SceneSettings","Studio environment"),K::Choice,0,1,1,{"neutral-studio"});
        add("environment.exposureEV","Environment",QT_TRANSLATE_NOOP("SceneSettings","Environment brightness (EV)"),K::Number,-16,16,.25);
        add("environment.rotation","Environment",QT_TRANSLATE_NOOP("SceneSettings","Environment rotation (degrees)"),K::Number,-180,180,1);
        add("background.mode","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Background"),K::Choice,0,1,1,{"solid","gradient"});
        add("background.topColor","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Top / solid color"),K::Color);
        add("background.bottomColor","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Bottom color"),K::Color);
        add("floor.enabled","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Shadow receiving floor"),K::Toggle);
        add("floor.color","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Floor color"),K::Color);
        add("floor.height","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Visual floor height (scene units)"),K::Number,-10000,10000,.01);
        add("floor.followPhysicsGround","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Follow MuJoCo ground height"),K::Toggle);
        add("grid.enabled","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Grid"),K::Toggle);
        add("grid.step","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Grid spacing (scene units)"),K::Number,.001,10000,.01);
        add("grid.intensity","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Grid intensity"),K::Number,0,1,.05);
        add("grid.axes","Background and floor",QT_TRANSLATE_NOOP("SceneSettings","Axes"),K::Toggle);
        add("camera.projection","Camera",QT_TRANSLATE_NOOP("SceneSettings","Projection"),K::Choice,0,1,1,{"perspective","orthographic"});
        add("camera.fovDegrees","Camera",QT_TRANSLATE_NOOP("SceneSettings","Field of view (degrees)"),K::Number,5,120,1);
        add("camera.orthographicHeight","Camera",QT_TRANSLATE_NOOP("SceneSettings","Orthographic height (scene units)"),K::Number,.01,10000,.1);
        add("post.enabled","Post-processing",QT_TRANSLATE_NOOP("SceneSettings","Post-processing"),K::Toggle);
        add("post.exposureEV","Post-processing",QT_TRANSLATE_NOOP("SceneSettings","Manual exposure (EV)"),K::Number,-16,16,.25);
        add("post.toneMapper","Post-processing",QT_TRANSLATE_NOOP("SceneSettings","Tone mapping"),K::Choice,0,1,1,{"aces","reinhard","srgb"});
        add("post.saturation","Post-processing",QT_TRANSLATE_NOOP("SceneSettings","Saturation"),K::Number,0,2,.05);
        add("post.contrast","Post-processing",QT_TRANSLATE_NOOP("SceneSettings","Contrast"),K::Number,.1,2,.05);
        add("post.bloom.enabled","Post-processing",QT_TRANSLATE_NOOP("SceneSettings","Bloom"),K::Toggle);
        add("post.bloom.threshold","Post-processing",QT_TRANSLATE_NOOP("SceneSettings","Bloom threshold (linear)"),K::Number,0,100,.1);
        add("post.bloom.intensity","Post-processing",QT_TRANSLATE_NOOP("SceneSettings","Bloom intensity"),K::Number,0,2,.05);
        return f;
    }();
    return result;
}
bool SceneVisualSettings::patch(const QJsonObject &changes,QString &error) {
    auto candidate=values;
    if(!merge(candidate,changes,error))return false;
    if(!candidate["version"].isDouble() || candidate["version"].toDouble()!=1) {
        error="visual.unsupported_version";return false;
    }
    for(const auto &field:fields()) {
        auto v=at(candidate,field.path.split('.'));
        bool valid=false;
        switch(field.kind) {
        case VisualFieldKind::Toggle:valid=v.isBool();break;
        case VisualFieldKind::Number:valid=v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble()>=field.minimum && v.toDouble()<=field.maximum;break;
        case VisualFieldKind::Choice:valid=v.isString() && field.choices.contains(v.toString());break;
        case VisualFieldKind::Color:valid=v.isString() && QRegularExpression("^#[0-9a-fA-F]{6}$").match(v.toString()).hasMatch() && QColor(v.toString()).isValid();break;
        }
        if(!valid) {error="visual.invalid_value: "+field.path;return false;}
    }
    values=candidate;error.clear();return true;
}
bool SceneVisualSettings::setValue(const QString &path,const QJsonValue &value,QString &error) {
    if(path.isEmpty()) { error="visual.unknown_parameter";return false; }
    QJsonObject change;put(change,path.split('.'),value);return patch(change,error);
}
bool SceneVisualSettings::fromJson(const QJsonObject &json,SceneVisualSettings &result,QString &error) {
    SceneVisualSettings candidate;
    if(!candidate.patch(json,error))return false;
    result=std::move(candidate);return true;
}
SceneVisualSettings SceneVisualSettings::preset(const QString &name) {
    SceneVisualSettings result;
    QString error;
    if(name=="studio")result.patch({{"lighting",QJsonObject{{"main",light(true,35,55,2400,true)},{"fill",light(true,-100,20,600,false)}}},{"environment",QJsonObject{{"exposureEV",1.0}}},{"background",QJsonObject{{"mode","gradient"}}}},error);
    else if(name=="silhouette")result.patch({{"silhouette",true},{"mode","unlit"},{"floor",QJsonObject{{"enabled",false}}},{"grid",QJsonObject{{"enabled",false}}},{"background",QJsonObject{{"topColor","#e5e5e5"},{"bottomColor","#e5e5e5"}}}},error);
    return result;
}
QJsonObject SceneVisualSettings::schema() {
    const QJsonObject versionSchema{{"type","integer"},{"enum",QJsonArray{1}}};
    QJsonObject root{{"type","object"},{"additionalProperties",false},
                     {"properties",QJsonObject{{"version",versionSchema}}}};
    for(const auto &field:fields()) {
        QJsonObject item;
        if(field.kind==VisualFieldKind::Toggle)item["type"]="boolean";
        else if(field.kind==VisualFieldKind::Number) {item["type"]="number";item["minimum"]=field.minimum;item["maximum"]=field.maximum;}
        else {item["type"]="string";if(field.kind==VisualFieldKind::Choice)item["enum"]=QJsonArray::fromStringList(field.choices);}
        auto path=field.path.split('.');QStringList schemaPath{"properties"};
        for(int i=0;i<path.size();++i) {schemaPath<<path[i];if(i+1<path.size())schemaPath<<"properties";}
        put(root,schemaPath,item);
    }
    // Every nested settings group is closed too, including when used by MCP.
    std::function<void(QJsonObject&)> close=[](QJsonObject&){};
    close=[&](QJsonObject &node) {
        if(!node.contains("properties"))return;
        node["type"]="object";node["additionalProperties"]=false;
        auto properties=node["properties"].toObject();
        for(auto it=properties.begin();it!=properties.end();++it)if(it->isObject()) {auto child=it->toObject();close(child);it.value()=child;}
        node["properties"]=properties;
    };
    close(root);return root;
}
} // namespace trickster
