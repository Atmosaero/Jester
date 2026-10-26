#include "settings/editor_appearance.h"
#include "ui/widgets/localized_ui.h"
#include "core/scene/runtime_error.h"
#include "ui/timeline/timeline.h"
#include "core/scene/edit_service.h"
#include "core/annotations/motion_annotations.h"
#include <QContextMenuEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
namespace trickster {
Timeline::Timeline(Document *d, QWidget *p) : QWidget(p), doc(d) {
    connect(&LocalizationService::instance(),&LocalizationService::languageChanged,this,[this] {rowCacheMotion=~quint64(0);update();});
    setMinimumHeight(130);
    setFocusPolicy(Qt::StrongFocus);
    setObjectName("timeline");
    scroll = new QScrollBar(Qt::Vertical, this);
    scroll->setObjectName("timelineTrackScroll");
    connect(scroll, &QScrollBar::valueChanged, this, QOverload<>::of(&Timeline::update));
    bindUiText(this,"toolTip","Click a track label to select its bone. Expand for Position / Rotation / Scale. Source "
               "tracks are read-only. Drag a green key; Ctrl-drag copies. Alt-wheel pans time; wheel scrolls "
               "tracks; Ctrl-wheel zooms.");
    setDocument(d);
}
void Timeline::setDocument(Document *d) {
    contextDrag.clear();contextOriginal={};selectingTime=false;
    if (doc)
        disconnect(doc, nullptr, this, nullptr);
    doc = d;
    rowCacheMotion = rowCacheMetadata = timeCacheRevision = ~quint64(0);timeCache.clear();
    connect(d, &Document::changed, this, [this] {
        if (selectionLayer != doc->scene.correction().id)
            syncSelection();
        update();
    });
    connect(d, &Document::selectionChanged, this, &Timeline::syncSelection);
    connect(d, &Document::annotationSelected, this, [this](QString id) {
        auto model=rows();
        scroll->setRange(0,std::max(0,int(model.size()-2)*rowHeight()-std::max(1,height()-bodyTop()-footerHeight())));
        for(int i=0;i<int(model.size());++i)if(model[i].kind==ContextTrack && model[i].reference==id){
            int y=rowY(i);
            if(y<bodyTop()+rowHeight()/2)scroll->setValue(std::max(0,scroll->value()+y-bodyTop()-rowHeight()/2));
            else if(y>height()-footerHeight()-rowHeight()/2)scroll->setValue(scroll->value()+y-height()+footerHeight()+rowHeight()/2);
            update();break;
        }
    });
    connect(d, &Document::sceneReplaced, this, [this] {
        contextDrag.clear();contextOriginal={};selectingTime=false;
        selectedKey = -1;
        dragKey = false;
        marks.clear();
        copiedLayers.clear();
        copiedPoses.clear();
        copiedMarks.clear();
        expanded.clear();
        scroll->setValue(0);
        fit();
        syncSelection();
    });
    marks.clear();
    copiedLayers.clear();
    copiedPoses.clear();
    copiedMarks.clear();
    selectedKey = -1;
    selectedRow = 0;
    dragKey = scrubbing = false;
    fit();
    syncSelection();
}
void Timeline::setOnlySelected(bool value) {
    onlySelected = value;
    syncSelection();
}
void Timeline::syncSelection() {
    if (selectingTrack) {
        update();
        return;
    }
    marks.clear();
    selectedKey = -1;
    filterNodes = doc->selection();
    dragKey = scrubbing = selectingKeys = false;
    selectionLayer = doc->scene.correction().id;
    selectedRow = doc->scene.correction().present ? 1 : 0;
    auto model = rows();
    for (int i = 2; i < int(model.size()); i++)
        if (model[i].node == doc->selected && model[i].channels == 7 && model[i].kind != ContactTrack) {
            selectedRow = i;
            break;
        }
    if (filterNodes.size() > 1)
        selectedRow = 2;
    if (onlySelected)
        scroll->setValue(0);
    else {
        int page = std::max(1, height()-bodyTop()-footerHeight());
        scroll->setRange(0, std::max(0, int(model.size() - 2) * rowHeight() - page));
        int top = std::max(0, (selectedRow - 2) * rowHeight());
        if (top < scroll->value())
            scroll->setValue(top);
        else if (top + rowHeight() > scroll->value() + page)
            scroll->setValue(top + rowHeight() - page);
    }
    update();
}
void Timeline::fit() {
    start = float(doc->scene.first);
    scale =
        float(std::max(1, width() - trackWidth - 30)) / std::max(1, doc->scene.last - doc->scene.first + 1);
    fitted = true;
    update();
}
float Timeline::x(float frame) const {
    return trackWidth + 12 + (frame - start) * scale;
}
int Timeline::frameAt(float position) const {
    return std::clamp(int(std::round((position - trackWidth - 12) / scale + start)), doc->scene.first,
                      doc->scene.last);
}
QString Timeline::expansionId(const Row &r) const {
    return QString::number(r.kind) + ":" + QString::number(r.node);
}
std::vector<Timeline::Row> Timeline::rows() const {
    if (rowCacheAnalysis == doc->analysisRevision && rowCacheMotion == doc->motionRevision && rowCacheMetadata == doc->metadataRevision &&
        rowCacheFilter == filterNodes && rowCacheExpanded == expanded && rowCacheSelected == onlySelected &&
        rowCacheSource == sourceDetails)
        return rowCache;
    const auto &s = doc->scene;
    std::vector<Row> result;
    QString clip = s.sourceClip >= 0 && s.clips ? (*s.clips)[s.sourceClip].name : uiText("Pose keys");
    result.push_back({Source, -1, 7, 0, (s.sourceClip >= 0 ? uiText("Source: ") : uiText("Pose bundle: ")) + clip, {}});
    result.push_back({Layer,
                      -1,
                      7,
                      0,
                      s.correction().present ? uiText("Layer: ") + s.correction().name : uiText("No correction layer"),
                      {}});
    std::set<int> shown;
    if (onlySelected) {
        for (int n : filterNodes)
            shown.insert(n);
    } else
        for (int n = 0; n < int(s.nodes.size()); n++)
            if (s.nodes[n].joint)
                shown.insert(n);
    if (filterNodes.size() > 1) {
        std::set<int> group;
        for (int n : filterNodes) {
            group.insert(n);
            if (s.isEffector(n)) {
                auto c = s.ikChain(n);
                if (c.root >= 0)
                    group.insert(c.root);
                if (c.mid >= 0)
                    group.insert(c.mid);
            }
        }
        std::vector<int> bones(group.begin(), group.end());
        Row r{s.correction().present ? Controller : SourceController,       -2,   7, 0,
              uiText("Selected (%1) [IK/FK TRS]").arg(filterNodes.size()), bones};
        result.push_back(r);
        if (expanded.contains(expansionId(r)))
            for (int n : bones)
                for (auto entry :
                     {std::pair<int, const char *>{1, "Position"}, {2, "Rotation"}, {4, "Scale"}})
                    result.push_back({s.correction().present ? LayerBone : SourceBone,
                                      n,
                                      entry.first,
                                      1,
                                      s.nodes[n].name + " / " + uiText(entry.second),
                                      {n}});
    }
    for (int n : shown) {
        auto chain = s.ikChain(n);
        bool ik = s.isEffector(n) && chain.root >= 0 && chain.mid >= 0;
        std::vector<int> affected = ik ? std::vector<int>{chain.root, chain.mid, n} : std::vector<int>{n};
        Row source{ik ? SourceController : SourceBone,
                   n,
                   7,
                   0,
                   s.nodes[n].name + (ik ? uiText(" [IK source]") : uiText(" [source]")),
                   affected};
        if (!s.correction().present) {
            result.push_back(source);
            if (expanded.contains(expansionId(source)))
                for (int bone : affected)
                    for (auto entry :
                         {std::pair<int, const char *>{1, "Position"}, {2, "Rotation"}, {4, "Scale"}})
                        result.push_back({SourceBone,
                                          bone,
                                          entry.first,
                                          1,
                                          (ik ? s.nodes[bone].name + " / " : QString()) + uiText(entry.second),
                                          {bone}});
            continue;
        }
        Row row{ik ? Controller : LayerBone,
                n,
                7,
                0,
                s.nodes[n].name + (ik ? uiText(" [IK chain TRS]") : " [TRS]"),
                affected};
        result.push_back(row);
        if (expanded.contains(expansionId(row)))
            for (int bone : affected)
                for (auto entry :
                     {std::pair<int, const char *>{1, "Position"}, {2, "Rotation"}, {4, "Scale"}})
                    result.push_back({LayerBone,
                                      bone,
                                      entry.first,
                                      1,
                                      (ik ? s.nodes[bone].name + " / " : QString()) + uiText(entry.second),
                                      {bone}});
        if (sourceDetails)
            result.push_back(source);
        if (sourceDetails && expanded.contains(expansionId(source)))
            for (int bone : affected)
                for (auto entry :
                     {std::pair<int, const char *>{1, "Position"}, {2, "Rotation"}, {4, "Scale"}})
                    result.push_back({SourceBone,
                                      bone,
                                      entry.first,
                                      1,
                                      (ik ? s.nodes[bone].name + " / " : QString()) + uiText(entry.second),
                                      {bone}});
    }
    for (const auto &c : s.contacts)
        if (!onlySelected || filterNodes.contains(c.node))
            result.push_back({ContactTrack, c.node, 7, 0, uiText("Contact: ") + s.nodes[c.node].name, {}});
    for (auto value : s.annotations) {
        auto a = value.toObject();
        bool shown = !onlySelected || a["nodes"].toArray().isEmpty();
        for (auto n : a["nodes"].toArray())
            if (filterNodes.contains(n.toInt()))
                shown = true;
        if (shown)
            result.push_back(
                {ContextTrack, -1, 0, 0, uiText("Context: ") + a["name"].toString(), {}, a["id"].toString()});
    }
    for (auto &curve : s.curves)
        if (!onlySelected || filterNodes.contains(curve.node))
            result.push_back(
                {CurveTrack,
                 curve.node,
                 0,
                 0,
                 uiText("Curve: %1 / %2 %3").arg(s.nodes[curve.node].name, curve.path).arg(curve.component),
                 {},
                 curve.id});
    for (auto entry : doc->motionProblems) {
        auto problem = entry.toObject();
        bool stale = doc->analysisMotionRevision != doc->motionRevision ||
                     doc->analysisMetadataRevision != doc->metadataRevision;
        result.push_back({DiagnosticTrack, -1, 0, 0,
                          uiText("%1%2").arg(stale ? "Stale: " : "Issue: ", problem["kind"].toString()),
                          {}, problem["problemId"].toString()});
    }
    rowCacheAnalysis = doc->analysisRevision;
    rowCache = result;
    rowCacheMotion = doc->motionRevision;
    rowCacheMetadata = doc->metadataRevision;
    rowCacheFilter = filterNodes;
    rowCacheExpanded = expanded;
    rowCacheSelected = onlySelected;
    rowCacheSource = sourceDetails;
    return result;
}
int Timeline::rowY(int i) const {
    return i<2?headerHeight()+i*rowHeight()+rowHeight()/2:bodyTop()+(i-2)*rowHeight()+rowHeight()/2-scroll->value();
}
int Timeline::rowHeight() const {return std::max(22,fontMetrics().height()+6);}
int Timeline::headerHeight() const {return std::max(24,fontMetrics().height()+8);}
int Timeline::footerHeight() const {return headerHeight();}
int Timeline::bodyTop() const {return headerHeight()+2*rowHeight();}
void Timeline::ensureRowVisible(int row) {
    if(row<2)return;
    const auto model=rows();const int page=std::max(1,height()-bodyTop()-footerHeight());
    scroll->setRange(0,std::max(0,int(model.size()-2)*rowHeight()-page));
    const int top=(row-2)*rowHeight();
    if(top<scroll->value())scroll->setValue(top);
    else if(top+rowHeight()>scroll->value()+page)scroll->setValue(top+rowHeight()-page);
}
void Timeline::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if(event->type()==QEvent::FontChange) {setMinimumHeight(std::max(130,bodyTop()+footerHeight()+rowHeight()));updateGeometry();update();}
}
int Timeline::rowAt(float y) const {
    if(y<headerHeight() || y>=height()-footerHeight())return -1;
    if(y<bodyTop())return int((y-headerHeight())/rowHeight());
    int row = 2 + int((y - bodyTop() + scroll->value()) / rowHeight());
    return row < int(rows().size()) ? row : -1;
}
int Timeline::contextTrackCenter(const QString &id) const {
    auto model = rows();
    for (int i = 0; i < int(model.size()); ++i)
        if (model[i].kind == ContextTrack && model[i].reference == id)
            return rowY(i);
    return -1;
}
int Timeline::trackCenter(int node, int channel, bool source) const {
    auto model = rows();
    for (int i = 2; i < int(model.size()); i++) {
        auto &r = model[i];
        bool src = r.kind == SourceBone || r.kind == SourceController;
        if (r.node == node && r.channels == channel && src == source && r.kind != ContactTrack)
            return rowY(i);
    }
    return -1;
}
bool Timeline::ensureTrackVisible(int node,int channel,bool source) {
    const auto model=rows();
    for(int row=2;row<int(model.size());++row) {
        const auto &track=model[row];const bool isSource=track.kind==SourceBone || track.kind==SourceController;
        if(track.node==node && track.channels==channel && isSource==source && track.kind!=ContactTrack) {ensureRowVisible(row);update();return true;}
    }
    return false;
}
bool Timeline::editable(const Row &r) const {
    const auto &s = doc->scene;
    if (s.compareOriginal)
        return false;
    if (r.kind == Source)
        return s.sourceClip < 0 && !s.correction().present;
    return (r.kind == Layer || r.kind == LayerBone || r.kind == Controller) && s.correction().present &&
           s.correction().enabled && s.correction().editing;
}
QList<float> Timeline::times(const Row &r) const {
    const auto &s = doc->scene;
    if(timeCacheRevision!=doc->motionRevision){timeCache.clear();timeCacheRevision=doc->motionRevision;}
    QString cacheKey=uiText("%1:%2:%3:%4").arg(r.kind).arg(r.node).arg(r.channels).arg(r.reference);
    for(int n:r.nodes)cacheKey+=":"+QString::number(n);
    auto cached=timeCache.constFind(cacheKey);if(cached!=timeCache.cend())return cached.value();
    QSet<float> out;
    if (r.kind == ContextTrack || r.kind == DiagnosticTrack)
        return {};
    if (r.kind == CurveTrack) {
        QList<float> times;
        for (auto &curve : s.curves)
            if (curve.id == r.reference)
                for (auto &key : curve.keys)
                    times.append(float(key.time * s.fps));
        timeCache[cacheKey]=times;return times;
    }
    bool source = r.kind == Source || r.kind == SourceBone || r.kind == SourceController;
    if (source) {
        if (s.sourceClip >= 0 && s.clips) {
            const auto &clip = (*s.clips)[s.sourceClip];
            for (const auto &ch : clip.channels) {
                int mask = ch.path == "translation" ? 1
                           : ch.path == "rotation"  ? 2
                           : ch.path == "scale"     ? 4
                                                    : 0;
                if (r.kind != Source &&
                    (!std::count(r.nodes.begin(), r.nodes.end(), ch.node) || !(r.channels & mask)))
                    continue;
                for (float time : ch.times)
                    out.insert(time * s.fps);
            }
            if (clip.channels.empty())
                for (auto it = clip.samples.cbegin(); it != clip.samples.cend(); ++it)
                    out.insert(float(it.key()) * s.fps / clip.sampleFps);
        } else
            for (int frame : s.keys.keys())
                out.insert(float(frame));
    } else if (r.kind != ContactTrack)
        for (auto it = s.correction().keys.cbegin(); it != s.correction().keys.cend(); ++it) {
            if (r.kind == Layer)
                out.insert(float(it.key()));
            else
                for (int n : r.nodes)
                    if (it->offsets.contains(n) && (it->channels.value(n, 7) & r.channels))
                        out.insert(float(it.key()));
        }
    auto values = out.values();
    std::sort(values.begin(), values.end());
    if(timeCache.size()>4096)timeCache.clear();timeCache[cacheKey]=values;
    return values;
}
int Timeline::keyAt(const Row &r, float position) const {
    if (!editable(r))
        return -1;
    int found = -1;
    float best = 8;
    for (float f : times(r))
        if (std::abs(x(f) - position) < best) {
            found = qRound(f);
            best = std::abs(x(f) - position);
        }
    return found;
}
QList<int> Timeline::navigationKeys() const {
    auto model = rows();
    const Row *scope = &model[doc->scene.correction().present ? 1 : 0];
    if (onlySelected && doc->selected >= 0)
        for (const auto &r : model)
            if (r.node == doc->selected && r.channels == 7 &&
                (doc->scene.correction().present ? (r.kind == LayerBone || r.kind == Controller)
                                                 : (r.kind == SourceBone || r.kind == SourceController))) {
                scope = &r;
                break;
            }
    QList<int> result;
    for (float f : times(*scope))
        if (!result.contains(qRound(f)))
            result.append(qRound(f));
    return result;
}
QString Timeline::scopeName(const Row &r) const {
    if (r.kind == Source) {
        if (doc->scene.sourceClip >= 0 && doc->scene.clips) {
            const auto &c = (*doc->scene.clips)[doc->scene.sourceClip];
            return uiText("Read-only source: ") + c.name +
                   (c.channels.empty() ? uiText(" / samples at %1 fps").arg(c.sampleFps)
                                       : uiText(" / channel key times"));
        }
        return uiText("Whole pose: all bones, pins and morphs");
    }
    if (r.kind == Layer)
        return uiText("Whole layer key: all keyed bones and morphs");
    QStringList names;
    for (int n : r.nodes)
        names << doc->scene.nodes[n].name;
    return names.join(", ") + " / " +
           (r.channels == 7   ? uiText("Position + Rotation + Scale")
            : r.channels == 1 ? uiText("Position")
            : r.channels == 2 ? uiText("Rotation")
                              : uiText("Scale"));
}
void Timeline::paintEvent(QPaintEvent *) {
    QElapsedTimer timer;
    timer.start();
    auto guard = qScopeGuard([&] { lastPaintMs = timer.nsecsElapsed() / 1e6; });
    if (fitted) {
        start = float(doc->scene.first);
        scale = float(std::max(1, width() - trackWidth - 30)) /
                std::max(1, doc->scene.last - doc->scene.first + 1);
    }
    auto model = rows();
    scroll->setGeometry(width() - 14, bodyTop(), 14, std::max(0, height()-bodyTop()-footerHeight()));
    scroll->setPageStep(std::max(1, height()-bodyTop()-footerHeight()));
    scroll->setRange(0, std::max(0, int(model.size() - 2) * rowHeight() - (height()-bodyTop()-footerHeight())));
    scroll->setVisible(scroll->maximum() > 0);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().color(QPalette::Base));
    int step = 1;
    while (step * scale < 45)
        step = step == 1 ? 5 : step * 2;
    for (int f = std::max(doc->scene.first, int(std::floor(start / step)) * step);
         f <= doc->scene.last && x(f) < width(); f += step) {
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(QPointF(x(f), headerHeight()), QPointF(x(f), height()-footerHeight()));
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QPointF(x(f) + 3, fontMetrics().ascent()+3), QString::number(f));
    }
    p.setPen(palette().color(QPalette::Highlight));
    for (auto item : doc->scene.annotations) {
        auto v = item.toObject();
        float a = x(v["start"].toDouble() * doc->scene.fps), b = x(v["end"].toDouble() * doc->scene.fps);
        if (b < trackWidth || a > width())
            continue;
        p.drawLine(QPointF(std::max(float(trackWidth), a), 24), QPointF(std::max(a + 3, b), 24));
        p.drawLine(QPointF(a, 20), QPointF(a, 30));
    }
    p.fillRect(QRect(0, 0, trackWidth, height()), palette().color(QPalette::Window));
    p.setPen(palette().color(QPalette::Text));
    p.drawText(QRect(8,0,trackWidth-16,headerHeight()),Qt::AlignVCenter,fontMetrics().elidedText(uiText("Tracks / operation scope"),Qt::ElideRight,trackWidth-16));
    for (int i = 0; i < int(model.size()); i++) {
        auto &r = model[i];
        int y = rowY(i);
        if (i >= 2 && (y < bodyTop() || y > height()-footerHeight()-1))
            continue;
        p.save();
        p.setClipRect(i < 2 ? QRect(0,headerHeight()+i*rowHeight(),width()-14,rowHeight())
                            : QRect(0,bodyTop(),width()-14, height()-bodyTop()-footerHeight()));
        bool selected = i == selectedRow || doc->selection().contains(r.node);
        if (selected)
            p.fillRect(QRect(0,y-rowHeight()/2,width()-14,rowHeight()-1), QColor(69, 85, 103, 95));
        p.setPen(palette().color(editable(r)?QPalette::Link:QPalette::PlaceholderText));
        QString label = r.label;
        if (i == 0 && doc->scene.sourceClip >= 0 && doc->scene.clips)
            label += uiText(" [read-only]");
        if (i == 1 && doc->scene.correction().present)
            label += editable(r) ? uiText(" [REC]") : uiText(" [locked]");
        if (i >= 2 && r.channels == 7 && r.kind != ContactTrack)
            label = (expanded.contains(expansionId(r)) ? "v " : "> ") + label;
        p.drawText(QRect(8+r.depth*12,y-rowHeight()/2,trackWidth-15-r.depth*12,rowHeight()),Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(label, Qt::ElideRight, trackWidth - 15 - r.depth * 12));
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(0, y+rowHeight()/2, width() - 14, y+rowHeight()/2);
        p.setClipRect(QRect(trackWidth, y - 10, width() - trackWidth - 15, 21));
        if (r.kind == ContextTrack)
            for (auto value : doc->scene.annotations) {
                auto a = value.toObject();
                if (a["id"] != r.reference)
                    continue;
                double from = contextDrag == r.reference ? contextStart : a["start"].toDouble(),
                       to = contextDrag == r.reference ? contextEnd : a["end"].toDouble();
                p.fillRect(
                    QRectF(x(float(from * doc->scene.fps)), y - 6,
                           std::max(4.f, x(float(to * doc->scene.fps)) - x(float(from * doc->scene.fps))),
                           12),
                    r.reference==doc->activeAnnotation ? overlayColor("notes") : overlayColor("notes",180));
            }
        if (r.kind == DiagnosticTrack)
            for (auto entry : doc->motionProblems) {
                auto issue = entry.toObject();
                if (issue["problemId"] != r.reference) continue;
                float a = x(float(issue["firstSeconds"].toDouble() * doc->scene.fps)),
                      b = x(float(issue["lastSeconds"].toDouble() * doc->scene.fps));
                p.fillRect(QRectF(a, y - 6, std::max(4.f, b - a), 12),
                           issue["status"] == "allowed" ? QColor(100, 170, 130, 160) : QColor(245, 95, 95, 160));
            }
        if (r.kind == CurveTrack)
            for (auto &curve : doc->scene.curves)
                if (curve.id == r.reference)
                    for (size_t k = 0; k + 1 < curve.keys.size(); ++k) {
                        float a = x(float(curve.keys[k].time * doc->scene.fps)),
                              b = x(float(curve.keys[k + 1].time * doc->scene.fps));
                        p.setPen(QColor(150, 190, 225));
                        p.drawLine(QPointF(a, y), QPointF(b, y));
                        if (b - a > 55)
                            p.drawText(QPointF(a + 8, y - 3),
                                       curve.keys[k].interpolation + " / " + curve.keys[k].handles);
                    }
        if (r.kind == ContactTrack)
            for (const auto &c : doc->scene.contacts)
                if (c.node == r.node) {
                    p.fillRect(QRectF(x(c.first), y - 5, std::max(2.f, x(c.last) - x(c.first)), 10),
                               QColor(78, 222, 164, 140));
                } else {
                } // Contact bands never participate in key hit-testing.
        if (r.kind != ContactTrack && r.kind != ContextTrack)
            for (float frame : times(r)) {
                float px = x(dragKey && marked(r, qRound(frame)) ? frame + dragTo - dragFrom : frame);
                if (px < trackWidth || px > width() - 14)
                    continue;
                if (editable(r)) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(marked(r, qRound(frame)) ? overlayColor("selection") : QColor(80, 205, 160));
                    p.drawPolygon(QPolygonF{
                        {px, float(y - 6)}, {px + 6, float(y)}, {px, float(y + 6)}, {px - 6, float(y)}});
                } else {
                    p.setPen(QColor(100, 168, 190));
                    p.drawLine(QPointF(px, y - 4), QPointF(px, y + 4));
                }
            }
        p.restore();
    }
    if (selectingKeys) {
        p.setPen(QColor(80, 180, 240));
        p.setBrush(QColor(60, 150, 220, 40));
        p.drawRect(keyRect.normalized());
    }
    if (doc->timeSelection) {
        auto a = doc->timeSelection->first * doc->scene.fps, b = doc->timeSelection->second * doc->scene.fps;
        p.fillRect(QRectF(x(float(a)), 0, x(float(b)) - x(float(a)), height()-footerHeight()),
                   QColor(80, 145, 220, 40));
    }
    p.setPen(QPen(palette().color(QPalette::Highlight), 2));
    p.drawLine(QPointF(x(doc->frame), headerHeight()-1), QPointF(x(doc->frame), height()-footerHeight()-1));
    p.fillRect(QRect(0, height()-footerHeight(), width(),footerHeight()), palette().color(QPalette::Window));
    p.setPen(palette().color(QPalette::Text));
    QString hint =
        dragKey ? (copyDrag ? uiText("Copy ") : uiText("Move ")) + scopeName(dragScope) + " -> " + QString::number(dragTo)
        : selectedRow < int(model.size()) ? scopeName(model[selectedRow])
                                          : uiText("Select a track");
    if (marks.size() > 1)
        hint = uiText("%1 selected key scopes | ").arg(marks.size()) + hint;
    p.drawText(8, height() - 7, QFontMetrics(p.font()).elidedText(hint, Qt::ElideRight, width() - 20));
}
void Timeline::mousePressEvent(QMouseEvent *e) {
    setFocus();
    if (e->button() != Qt::LeftButton)
        return;
    auto model = rows();
    if ((e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) ==
            (Qt::ControlModifier | Qt::ShiftModifier) &&
        e->position().x() >= trackWidth) {
        selectingTime = true;
        selectionAnchor = double(frameAt(float(e->position().x()))) / doc->scene.fps;
        doc->selectTime(selectionAnchor, selectionAnchor);
        return;
    }
    int row = rowAt(e->position().y());
    if (row < 0) {
        if (e->position().y() >= headerHeight() || e->position().x() < trackWidth)
            return;
        scrubbing = true;
        doc->play(false);
        doc->seek(frameAt(float(e->position().x())));
        return;
    }
    Row scope = model[row];
    if (scope.kind == DiagnosticTrack) {
        for (auto entry : doc->motionProblems) {
            auto issue = entry.toObject();
            if (issue["problemId"] != scope.reference) continue;
            doc->play(false);
            const double seconds=issue["peakSeconds"].isDouble()
                                     ? issue["peakSeconds"].toDouble() : issue["firstSeconds"].toDouble();
            doc->seek(std::clamp(qRound(seconds * doc->scene.fps),
                                 doc->scene.first, doc->scene.last));
            QSet<int> bones;
            for (auto node : issue["nodes"].toArray()) bones.insert(node.toInt());
            if (!bones.empty()) doc->setSelection(bones);
        }
        return;
    }
    if (scope.kind == ContextTrack) {
        for (auto value : doc->scene.annotations) {
            auto a = value.toObject();
            if (a["id"] != scope.reference)
                continue;
            if (e->position().x() < trackWidth) {
                doc->selectAnnotation(scope.reference);
                return;
            }
            contextStart = a["start"].toDouble();
            contextEnd = a["end"].toDouble();
            float startX = x(float(contextStart * doc->scene.fps)),
                  endX = x(float(contextEnd * doc->scene.fps));
            if (e->position().x() < startX - 7 || e->position().x() > endX + 7)
                return;
            contextDrag = scope.reference;
            contextOriginal = a;
            contextPress = (e->position().x() - trackWidth - 12) / scale / doc->scene.fps +
                           double(start) / doc->scene.fps;
            contextEdge = std::abs(endX-startX) < 14 ? 0 : std::abs(e->position().x() - startX) < 7 ? -1
                          : std::abs(e->position().x() - endX) < 7 ? 1
                                                                   : 0;
            dragRevision = doc->revision;
            doc->play(false);
            doc->seek(std::clamp(qRound(contextStart * doc->scene.fps), doc->scene.first, doc->scene.last));
            update();
            return;
        }
    }
    if (scope.kind == CurveTrack) {
        doc->select(scope.node);
        emit graphRequested();
        return;
    }
    selectedRow = row;
    emit scopeChanged(scopeName(scope));
    selectedKey = -1;
    if (e->position().x() < trackWidth) {
        marks.clear();
        selectingTrack = true;
        if (scope.node >= 0) {
            if (e->modifiers() & Qt::ShiftModifier)
                doc->select(scope.node, true);
            else
                doc->select(scope.node);
        }
        selectingTrack = false;
        if (scope.node >= 0) {
            if (e->modifiers() & Qt::ShiftModifier)
                syncSelection();
            else
                selectTrack(scope.node, scope.channels,
                            scope.kind == SourceBone || scope.kind == SourceController);
        }
        if (scope.channels == 7 && row >= 2 && scope.kind != ContactTrack) {
            auto id = expansionId(scope);
            if (expanded.contains(id))
                expanded.remove(id);
            else
                expanded.insert(id);
        }
        setToolTip(scopeName(scope) +
                   (editable(scope) ? uiText(" | Editable") : uiText(" | Read-only; create/record a correction to edit")));
        update();
        return;
    }
    doc->play(false);
    int key = keyAt(scope, float(e->position().x()));
    if (key >= 0) {
        bool exists = marked(scope, key);
        if (e->modifiers() & Qt::ShiftModifier) {
            if (exists)
                marks.erase(std::remove_if(marks.begin(), marks.end(),
                                           [&](const Mark &m) {
                                               return m.frame == key && m.scope.kind == scope.kind &&
                                                      m.scope.node == scope.node &&
                                                      m.scope.channels == scope.channels;
                                           }),
                            marks.end());
            else
                marks.push_back({scope, key});
            selectedKey = key;
            selectionRevision = doc->revision;
            update();
            return;
        }
        if (!exists) {
            marks.clear();
            marks.push_back({scope, key});
        }
        selectedKey = key;
        dragKey = true;
        dragFrom = dragTo = key;
        dragScope = scope;
        dragRevision = doc->revision;
        dragLayer = doc->scene.correction().id;
        selectionRevision = doc->revision;
        copyDrag = e->modifiers() & Qt::ControlModifier;
        doc->seek(key);
    } else if (e->modifiers() & Qt::ShiftModifier) {
        selectingKeys = true;
        keyRect = QRectF(e->position(), e->position());
        dragRevision = doc->revision;
        dragLayer = doc->scene.correction().id;
    } else {
        marks.clear();
        scrubbing = true;
        doc->seek(frameAt(float(e->position().x())));
    }
    update();
}
void Timeline::mouseMoveEvent(QMouseEvent *e) {
    if (selectingTime) {
        doc->selectTime(selectionAnchor, double(frameAt(float(e->position().x()))) / doc->scene.fps);
        return;
    }
    if (!contextDrag.isEmpty()) {
        double t = (e->position().x() - trackWidth - 12) / scale / doc->scene.fps +
                   double(start) / doc->scene.fps,
               delta = t - contextPress;
        double a = contextOriginal["start"].toDouble(), b = contextOriginal["end"].toDouble();
        contextStart = contextEdge == 1 ? a : contextEdge == -1 ? std::min(a + delta, b) : a + delta;
        contextEnd = contextEdge == -1 ? b : contextEdge == 1 ? std::max(a, b + delta) : b + delta;
        update();
        return;
    }
    if (selectingKeys) {
        keyRect.setBottomRight(e->position());
        update();
        return;
    }
    if (dragKey) {
        dragTo = frameAt(float(e->position().x()));
        update();
    } else if (scrubbing)
        doc->seek(frameAt(float(e->position().x())));
    else {
        auto model = rows();
        int row = rowAt(e->position().y());
        if (row >= 0)
            setToolTip(scopeName(model[row]) + (editable(model[row]) ? uiText(" | Editable") : uiText(" | Read-only")));
    }
}
void Timeline::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    if (selectingTime) {
        selectingTime = false;
        return;
    }
    if (!contextDrag.isEmpty()) {
        auto id = contextDrag;
        contextDrag.clear();
        if (doc->revision == dragRevision &&
            (contextStart != contextOriginal["start"].toDouble() || contextEnd != contextOriginal["end"].toDouble()))
            try {
                EditService::execute(
                    *doc, "motion_annotations",
                    {{"operation", "set"},
                     {"id", id},
                     {"annotation", QJsonObject{{"start", contextStart}, {"end", contextEnd}}}},
                    "Move/stretch motion context");
            } catch (const std::exception &error) {
                emit doc->feedback(error.what());
            }
        doc->selectAnnotation(id);
        update();
        return;
    }
    if (selectingKeys && doc->revision == dragRevision && doc->scene.correction().id == dragLayer) {
        auto rect = keyRect.normalized();
        auto model = rows();
        for (int i = 0; i < int(model.size()); i++)
            if (editable(model[i]) && rowY(i) >= (i < 2 ? headerHeight() : bodyTop()) && rowY(i) < height()-footerHeight())
                for (float time : times(model[i]))
                    if (rect.contains(QPointF(x(time), rowY(i))) && !marked(model[i], qRound(time)))
                        marks.push_back({model[i], qRound(time)});
        selectionRevision = doc->revision;
        selectedKey = marks.empty() ? -1 : marks.back().frame;
    }
    if (dragKey && dragFrom != dragTo) {
        if (doc->revision == dragRevision && doc->scene.correction().id == dragLayer)
            transformSelection(dragTo - dragFrom, copyDrag);
        else {
            marks.clear();
            selectedKey = -1;
            emit doc->feedback("Source or layer changed; key drag cancelled.");
        }
    }
    dragKey = scrubbing = selectingKeys = false;
    update();
}
void Timeline::wheelEvent(QWheelEvent *e) {
    if (!(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && scroll->maximum() > 0) {
        scroll->setValue(scroll->value() - e->angleDelta().y() / 2);
        return;
    }
    float px = float(e->position().x()), old = (px - trackWidth - 12) / scale + start;
    fitted = false;
    if (e->modifiers() & Qt::ControlModifier) {
        scale = std::clamp(scale * std::exp(e->angleDelta().y() * .0015f), .01f, 100.f);
        start = old - (px - trackWidth - 12) / scale;
    } else
        start -= e->angleDelta().y() * .1f / scale;
    start = std::clamp(start, float(doc->scene.first) - 10, float(doc->scene.last));
    update();
}
bool Timeline::marked(const Row &r, int frame) const {
    return std::any_of(marks.begin(), marks.end(), [&](const Mark &m) {
        return m.frame == frame && m.scope.kind == r.kind && m.scope.node == r.node &&
               m.scope.channels == r.channels;
    });
}
QJsonArray Timeline::keyRequests() const {
    QJsonArray result;
    for (const auto &m : marks) {
        QJsonObject r{{"frame", m.frame}};
        if (m.scope.kind != Source && m.scope.kind != Layer) {
            QJsonArray nodes;
            for (int n : m.scope.nodes)
                nodes.append(n);
            r["nodes"] = nodes;
            r["channels"] = m.scope.channels;
        }
        result.append(r);
    }
    return result;
}
bool Timeline::transformSelection(int delta, bool copy, bool remove) {
    if (marks.empty())
        return false;
    if (selectionRevision != doc->revision) {
        marks.clear();
        selectedKey = -1;
        emit doc->feedback("Animation changed; select the keys again.");
        return false;
    }
    try {
        bool raw = marks.front().scope.kind == Source;
        for (auto &m : marks)
            if (!editable(m.scope) || (m.scope.kind == Source) != raw)
                throw trickster::RuntimeError("Choose editable keys from the same recording target");
        EditService::execute(*doc, raw ? "timeline" : "motion_correction",
                             {{"operation", "keys_transform"},
                              {"keys", keyRequests()},
                              {"delta", delta},
                              {"copy", copy},
                              {"remove", remove}},
                             remove ? "Delete selected keys"
                             : copy ? "Copy selected keys"
                                    : "Move selected keys");
        if (remove) {
            marks.clear();
            selectedKey = -1;
        } else {
            for (auto &m : marks)
                m.frame += delta;
            selectedKey = dragFrom + delta;
            doc->seek(std::clamp(selectedKey, doc->scene.first, doc->scene.last));
        }
        selectionRevision = doc->revision;
        return true;
    } catch (const std::exception &e) {
        emit doc->feedback(translatedDiagnostic(QString::fromUtf8(e.what())));
        return false;
    }
}
bool Timeline::selectTrack(int node, int channels, bool source) {
    if (channels != 1 && channels != 2 && channels != 4 && channels != 7)
        return false;
    if (node >= int(doc->scene.nodes.size()) || node < -2)
        return false;
    if (node >= 0) {
        selectingTrack = true;
        doc->select(node);
        selectingTrack = false;
        filterNodes = doc->selection();
    }
    bool src = source || !doc->scene.correction().present;
    if (source)
        sourceDetails = true;
    if (channels != 7 && node >= 0) {
        Row parent{src ? (doc->scene.isEffector(node) ? SourceController : SourceBone)
                       : (doc->scene.isEffector(node) ? Controller : LayerBone),
                   node,
                   7,
                   0,
                   {},
                   {}};
        expanded.insert(expansionId(parent));
    }
    auto model = rows();
    for (int i = 0; i < int(model.size()); i++) {
        bool isSource =
            model[i].kind == Source || model[i].kind == SourceBone || model[i].kind == SourceController;
        if ((node == -1 ? (i == (source ? 0 : 1))
                        : model[i].node == node && model[i].channels == channels && isSource == src)) {
            selectedRow = i;
            marks.clear();
            selectedKey = -1;
            int page = std::max(1, height()-bodyTop()-footerHeight());
            scroll->setRange(0, std::max(0, int(model.size() - 2) * rowHeight() - page));
            int top = std::max(0, (i - 2) * rowHeight());
            if (top < scroll->value() || top + rowHeight() > scroll->value() + page)
                scroll->setValue(top);
            emit scopeChanged(insertionScope());
            update();
            return true;
        }
    }
    return false;
}
QString Timeline::insertionScope() const {
    auto model = rows();
    return selectedRow < int(model.size()) ? scopeName(model[selectedRow]) : "Choose a track";
}
void Timeline::insertKey(bool neutral) {
    auto model = rows();
    if (selectedRow >= int(model.size()))
        return;
    auto scope = model[selectedRow];
    if (scope.kind == ContactTrack) {
        emit doc->feedback("Contact bands are read-only. Choose a bone/channel track to insert keys.");
        return;
    }
    auto &s = doc->scene;
    if (s.compareOriginal ||
        (s.correction().present && (!s.correction().enabled || !s.correction().editing))) {
        emit doc->feedback("Enable recording before inserting keys");
        return;
    }
    if (!s.correction().present)
        for (const auto &id : QSet<QString>(expanded)) {
            auto fields = id.split(':');
            int kind = fields[0].toInt();
            if (kind == SourceBone || kind == SourceController)
                expanded.insert(QString::number(kind == SourceBone ? LayerBone : Controller) + ":" +
                                fields[1]);
        }
    QJsonObject args{{"operation", "insert"}, {"whole", true}};
    if (scope.kind != Source && scope.kind != Layer) {
        QJsonArray nodes;
        for (int n : scope.nodes)
            nodes.append(n);
        args["nodes"] = nodes;
        args["channels"] = scope.channels;
        args["includeChain"] = false;
    }
    try {
        if (neutral) {
            args["operation"] = "key_reset";
            EditService::execute(*doc, "motion_correction", args, "Neutral " + scopeName(scope));
        } else
            EditService::execute(*doc, "timeline", args, "Insert " + scopeName(scope));
        // Creation of the first correction changes row kinds; retain the chosen component scope.
        auto updated = rows();
        if (scope.kind == Source && s.correction().present)
            selectedRow = 1;
        else
            for (int i = 0; i < int(updated.size()); i++)
                if (updated[i].node == scope.node && updated[i].channels == scope.channels &&
                    updated[i].kind != SourceBone && updated[i].kind != SourceController) {
                    selectedRow = i;
                    break;
                }
        int page = std::max(1, height()-bodyTop()-footerHeight());
        scroll->setRange(0, std::max(0, int(updated.size() - 2) * rowHeight() - page));
        int top = std::max(0, (selectedRow - 2) * rowHeight());
        if (top < scroll->value())
            scroll->setValue(top);
        else if (top + rowHeight() > scroll->value() + page)
            scroll->setValue(top + rowHeight() - page);
        emit doc->feedback((neutral ? "Neutral: " : "Key: ") + scopeName(scope));
        update();
    } catch (const std::exception &e) {
        emit doc->feedback(translatedDiagnostic(QString::fromUtf8(e.what())));
    }
}
void Timeline::copy() {
    auto model = rows();
    if (marks.empty() && selectedRow < int(model.size()) && editable(model[selectedRow])) {
        int f = selectedKey >= 0 ? selectedKey : doc->frame;
        if (keyAt(model[selectedRow], x(f)) == f)
            marks.push_back({model[selectedRow], f});
        selectionRevision = doc->revision;
    }
    if (marks.empty())
        return;
    if (selectionRevision != doc->revision) {
        marks.clear();
        selectedKey = -1;
        emit doc->feedback("Animation changed; select the keys again before copying.");
        return;
    }
    try {
        copiedLayers.clear();
        copiedPoses.clear();
        copiedMarks = marks;
        if (marks.front().scope.kind == Source)
            for (auto &m : marks)
                copiedPoses[m.frame] = doc->scene.keys.value(m.frame);
        else {
            copiedLayers = EditService::selectedCorrectionKeys(doc->scene, keyRequests());
            clipboardWorld = doc->scene.correction().world;
        }
        emit doc->feedback(
            uiText("Copied %1 key scopes; paste retains their bones/channels and relative times.")
                .arg(marks.size()));
    } catch (const std::exception &e) {
        emit doc->feedback(translatedDiagnostic(QString::fromUtf8(e.what())));
    }
}
void Timeline::paste() {
    auto model = rows();
    if (selectedRow >= int(model.size()) || !editable(model[selectedRow]))
        return;
    try {
        Scene staged = doc->scene;
        if (!copiedLayers.isEmpty()) {
            if (!staged.correction().present || clipboardWorld != staged.correction().world)
                throw trickster::RuntimeError("Choose a correction layer using the copied coordinate space");
            EditService::pasteCorrectionKeys(staged, copiedLayers, doc->frame - copiedLayers.firstKey());
        } else if (!copiedPoses.isEmpty()) {
            if (model[selectedRow].kind != Source)
                throw trickster::RuntimeError("Whole-pose keys must be pasted on the whole-pose track");
            int delta = doc->frame - copiedPoses.firstKey();
            for (auto i = copiedPoses.cbegin(); i != copiedPoses.cend(); ++i) {
                int f = i.key() + delta;
                if (f < staged.first || f > staged.last || staged.keys.contains(f))
                    throw trickster::RuntimeError("Pose destination is occupied or outside the clip");
                staged.keys[f] = i.value();
                staged.keys[f].id.clear();
            }
        } else
            return;
        staged.evaluate(float(doc->frame));
        QString error;
        if (!staged.validate(&error))
            throw trickster::RuntimeError(error.toStdString());
        int origin = !copiedLayers.isEmpty() ? copiedLayers.firstKey() : copiedPoses.firstKey();
        int shift = doc->frame - origin;
        doc->edit("Paste selected keys", [&](Scene &s) { s = std::move(staged); }, false);
        marks = copiedMarks;
        for (auto &m : marks)
            m.frame += shift;
        selectedKey = doc->frame;
        selectionRevision = doc->revision;
        update();
    } catch (const std::exception &e) {
        emit doc->feedback(translatedDiagnostic(QString::fromUtf8(e.what())));
    }
}
void Timeline::keyPressEvent(QKeyEvent *e) {
    auto model = rows();
    if (e->key() == Qt::Key_Delete) {
        transformSelection(0, false, true);
        selectedKey = -1;
    } else if (e->matches(QKeySequence::Copy))
        copy();
    else if (e->matches(QKeySequence::Paste))
        paste();
    else if (e->key() == Qt::Key_Home)
        fit();
    else if (e->key() == Qt::Key_Escape) {
        dragKey = scrubbing = selectingKeys = false;
        update();
    } else
        QWidget::keyPressEvent(e);
}
void Timeline::contextMenuEvent(QContextMenuEvent *e) {
    auto model = rows();
    int row = rowAt(e->pos().y());
    if (row < 0)
        return;
    selectedRow = row;
    auto scope = model[row];
    selectedKey = keyAt(scope, float(e->pos().x()));
    if (selectedKey >= 0 && !marked(scope, selectedKey)) {
        marks = {{scope, selectedKey}};
        selectionRevision = doc->revision;
    }
    QMenu menu;
    auto addNote = addTranslatedAction(&menu,"Add note");
    auto context = addTranslatedAction(&menu,"Notes panel");
    QAction *editNote = scope.kind==ContextTrack ? addTranslatedAction(&menu,"Edit note") : nullptr;
    auto title = menu.addAction(scopeName(scope));
    title->setEnabled(false);
    auto cp = addTranslatedAction(&menu,"Copy selected key");
    cp->setEnabled(selectedKey >= 0);
    auto ps = addTranslatedAction(&menu,"Paste at playhead");
    ps->setEnabled(editable(scope) && (!copiedPoses.isEmpty() || !copiedLayers.isEmpty()));
    auto del = addTranslatedAction(&menu,"Delete selected key");
    del->setEnabled(selectedKey >= 0);
    if (!editable(scope)) {
        auto readonly = addTranslatedAction(&menu,"Read-only source / locked layer");
        readonly->setEnabled(false);
    }
    auto interpolation=menu.addMenu("Selected interpolation");
    interpolation->setEnabled(editable(scope)&&!marks.empty());
    for(QString mode:{"Step","Linear","Bezier","Hermite","Bounce","Elastic","Back"})
        interpolation->addAction(mode)->setData("interpolation:"+mode);
    auto details = addTranslatedAction(&menu,"Show source detail tracks");
    details->setCheckable(true);
    details->setChecked(sourceDetails);
    auto fitAction = addTranslatedAction(&menu,"Fit time range");
    auto chosen = menu.exec(e->globalPos());
    if(chosen && chosen->data().toString().startsWith("interpolation:")) {
        try{EditService::execute(*doc,"animation_curves",{{"operation","interpolation"},{"selection",keyRequests()},
            {"layer",doc->scene.correction().present?doc->scene.correction().id:QString()},
            {"interpolation",chosen->text()}},"Selected interpolation");}
        catch(const std::exception &error){emit doc->feedback(error.what());}
    }
    else if (chosen == addNote) {
        if(auto action=window()->findChild<QAction*>("addNoteAction"))action->trigger();
    }
    else if (editNote && chosen == editNote) doc->selectAnnotation(scope.reference);
    else if (chosen == context)
        editContext();
    else if (chosen == cp)
        copy();
    else if (chosen == ps)
        paste();
    else if (chosen == del)
        transformSelection(0, false, true);
    else if (chosen == details) {
        sourceDetails = details->isChecked();
        syncSelection();
    } else if (chosen == fitAction)
        fit();
    update();
}
void Timeline::editContext() { emit notesRequested(); }
} // namespace trickster
