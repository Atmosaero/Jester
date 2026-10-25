#include "edit_service.h"
#include "editor_api_util.h"
#include <QUuid>
#include <algorithm>
namespace trickster {
using namespace api_detail;
namespace {
void merge(CorrectionKey &to, const CorrectionKey &from, bool collision) {
    for (int n : from.offsets.keys()) {
        int bits = from.channels.value(n, 7), old = to.offsets.contains(n) ? to.channels.value(n, 7) : 0;
        require(!collision || !(old & bits), "Destination already contains selected bone channels");
        Transform neutral;
        neutral.s = {0, 0, 0};
        auto t = to.offsets.value(n, neutral), v = from.offsets[n];
        if (bits & 1)
            t.t = v.t;
        if (bits & 2)
            t.r = v.r;
        if (bits & 4)
            t.s = v.s;
        to.offsets[n] = t;
        to.channels[n] = old | bits;
    }
    for (int n : from.morphOffsets.keys()) {
        require(!collision || !to.morphOffsets.contains(n),
                "Destination already contains selected morph keys");
        to.morphOffsets[n] = from.morphOffsets[n];
    }
}
void recording(const Scene &s) {
    require(!s.compareOriginal && s.correction().present && s.correction().enabled && s.correction().editing,
            "Enable recording on the active layer before editing keys");
}
} // namespace
QMap<int, CorrectionKey> EditService::selectedCorrectionKeys(const Scene &s, const QJsonArray &requests) {
    require(!requests.isEmpty() && requests.size() <= 4096, "Choose 1-4096 key scopes");
    QMap<int, CorrectionKey> selected;
    for (auto v : requests) {
        auto r = v.toObject();
        int frame = r["frame"].toInt(-1), mask = r["channels"].toInt(7);
        require(mask >= 1 && mask <= 7, "Invalid channel mask");
        require(s.correction().keys.contains(frame), "A selected key no longer exists");
        auto source = s.correction().keys[frame];
        if (!r.contains("node") && !r.contains("nodes")) {
            merge(selected[frame], source, false);
            selected[frame].id = source.id;
        } else {
            QJsonArray nodes = r["nodes"].toArray();
            if (r.contains("node"))
                nodes.append(r["node"]);
            require(!nodes.isEmpty(), "Empty bone scope");
            CorrectionKey part;
            for (auto value : nodes) {
                int n = node(s, value),
                    bits = source.offsets.contains(n) ? source.channels.value(n, 7) & mask : 0;
                if (bits) {
                    part.offsets[n] = source.offsets[n];
                    part.channels[n] = bits;
                }
            }
            if (!part.offsets.isEmpty())
                merge(selected[frame], part, false);
        }
    }
    require(!selected.isEmpty(), "No keys in the selected channels");
    return selected;
}
void EditService::pasteCorrectionKeys(Scene &s, const QMap<int, CorrectionKey> &input, int delta) {
    recording(s);
    auto output = s.correction().keys;
    for (auto i = input.cbegin(); i != input.cend(); ++i) {
        qint64 frame = qint64(i.key()) + delta;
        require(frame >= s.first && frame <= s.last, "Key selection would leave the clip range");
        merge(output[int(frame)], i.value(), true);
        if (output[int(frame)].id.isEmpty())
            output[int(frame)].id = i->id;
    }
    s.correction().keys = std::move(output);
}
void EditService::transformKeys(Scene &s, const QJsonArray &requests, int delta, bool copy, bool remove,
                                bool raw) {
    require(!s.compareOriginal, "Leave source comparison before editing keys");
    if (raw) {
        require(s.sourceClip < 0 && !s.correction().present,
                "Only the explicit whole-pose track can edit raw keys");
        require(!requests.isEmpty() && requests.size() <= 4096, "Choose 1-4096 keys");
        QMap<int, Key> selected;
        auto output = s.keys;
        for (auto v : requests) {
            auto r = v.toObject();
            require(!r.contains("node") && !r.contains("nodes"), "Raw pose keys are whole-pose bundles");
            int f = r["frame"].toInt(-1);
            require(output.contains(f), "A selected pose key no longer exists");
            selected[f] = output[f];
        }
        if (remove || !copy)
            for (int f : selected.keys())
                output.remove(f);
        if (!remove)
            for (auto i = selected.cbegin(); i != selected.cend(); ++i) {
                qint64 f = qint64(i.key()) + delta;
                require(f >= s.first && f <= s.last, "Key selection would leave the clip range");
                require(!output.contains(int(f)), "Destination already contains a pose key");
                output[int(f)] = i.value();
                if (copy)
                    output[int(f)].id.clear();
            }
        s.keys = std::move(output);
        return;
    }
    recording(s);
    auto selected = selectedCorrectionKeys(s, requests);
    Scene staged = s.motionOnlyCopy();
    if (remove || !copy)
        for (auto i = selected.cbegin(); i != selected.cend(); ++i) {
            auto &key = staged.correction().keys[i.key()];
            for (int n : i->offsets.keys()) {
                int left = key.channels.value(n, 7) & ~i->channels.value(n, 7);
                if (left)
                    key.channels[n] = left;
                else {
                    key.offsets.remove(n);
                    key.channels.remove(n);
                }
            }
            for (int n : i->morphOffsets.keys())
                key.morphOffsets.remove(n);
            if (key.offsets.isEmpty() && key.morphOffsets.isEmpty())
                staged.correction().keys.remove(i.key());
        }
    if (!remove) {
        if (copy)
            for (auto &key : selected)
                key.id.clear();
        pasteCorrectionKeys(staged, selected, delta);
    }
    s.correction().keys = staged.correction().keys;
}
} // namespace trickster
