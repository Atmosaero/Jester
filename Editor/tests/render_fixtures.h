#pragma once
#include "core/scene/core.h"
#include <QMap>
#include <algorithm>
#include <cmath>
#include <map>
namespace trickster::render_fixture {
inline void subdivide(Mesh &mesh) {
    QMap<quint64,unsigned> edges;
    auto midpoint=[&](unsigned a,unsigned b) {
        const quint64 key=(quint64(std::min(a,b))<<32)|std::max(a,b);
        if(edges.contains(key))return edges[key];
        const auto x=mesh.vertices[a],y=mesh.vertices[b];
        Vertex v;v.p=(x.p+y.p)*.5f;v.normal=(x.normal+y.normal).normalized();
        v.uv=(x.uv+y.uv)*.5f;v.uv1=(x.uv1+y.uv1)*.5f;
        v.tangent=QVector4D((x.tangent.toVector3D()+y.tangent.toVector3D()).normalized(),x.tangent.w());
        std::map<int,float> weights;
        for(int i=0;i<8;++i) {weights[x.joints[i]]+=x.weights[i]*.5f;weights[y.joints[i]]+=y.weights[i]*.5f;}
        std::vector<std::pair<int,float>> sorted(weights.begin(),weights.end());
        std::sort(sorted.begin(),sorted.end(),[](auto a,auto b){return a.second>b.second;});
        v.weights.fill(0);float sum=0;
        for(size_t i=0;i<std::min(size_t(8),sorted.size());++i) {v.joints[i]=sorted[i].first;v.weights[i]=sorted[i].second;sum+=v.weights[i];}
        if(sum>0)for(auto &w:v.weights)w/=sum;
        const unsigned index=unsigned(mesh.vertices.size());mesh.vertices.push_back(v);edges[key]=index;return index;
    };
    std::vector<unsigned> indices;indices.reserve(mesh.indices.size()*4);
    for(size_t i=0;i+2<mesh.indices.size();i+=3) {
        auto a=mesh.indices[i],b=mesh.indices[i+1],c=mesh.indices[i+2];
        auto ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
        for(auto index:{a,ab,ca,ab,b,bc,ca,bc,c,ab,bc,ca})indices.push_back(index);
    }
    mesh.indices=std::move(indices);
}
inline Scene scene(const QString &kind) {
    auto s=Scene::mannequin();s.name="Jester benchmark / "+kind;
    s.first=0;s.last=90;s.fps=30;s.keys.clear();
    if(kind=="heavy-morph") {
        auto &mesh=s.meshes[0];
        for(int level=0;level<3;++level)subdivide(mesh);
        mesh.node=0;s.nodes[0].morphRest={0,0};
        for(int k=0;k<2;++k) {
            MorphTarget target;target.name="Synthetic deformation "+QString::number(k);
            for(const auto &v:mesh.vertices) {
                const float wave=std::sin(v.p.y()*4+float(k));
                target.positions.push_back({.025f*wave,.01f*std::sin(v.p.x()*3),.015f*wave});
                target.normals.push_back({.01f*wave,.005f,0});
                target.tangents.push_back({0,.01f*wave,.005f});
            }
            mesh.targets.push_back(std::move(target));
        }
        s.morphs[0]={0,0};
    } else if(kind=="multi-mesh") {
        const auto original=s.meshes[0];s.meshes.clear();
        for(int i=0;i<4;++i) {
            auto mesh=original;
            for(auto &v:mesh.vertices)v.p.setX(v.p.x()+(float(i)-1.5f)*.65f);
            mesh.color=QColor::fromHsv(35+i*45,95,180);s.meshes.push_back(std::move(mesh));
        }
    }
    const auto rest=s.pose;
    for(int frame:{0,30,60,90}) {
        s.pose=rest;
        s.pose[7].r=QQuaternion::fromAxisAndAngle(0,0,1,float(frame==30?35:frame==60?-20:0));
        s.pose[0].r=QQuaternion::fromAxisAndAngle(0,1,0,float(frame==30?12:frame==60?-12:0));
        if(kind=="heavy-morph")s.morphs[0]={frame==30?1.f:0.f,frame==60?.8f:0.f};
        s.keys[frame]={s.pose,{},s.morphs};
    }
    s.evaluate(0);return s;
}
} // namespace trickster::render_fixture
