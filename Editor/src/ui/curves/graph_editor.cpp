#include "core/scene/edit_service.h"
#include "ui/window/window.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
namespace trickster {
class CurveEnumDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &index) const override {
        auto box = new QComboBox(parent);
        box->addItems(index.column() == 2
                          ? QStringList{"Step", "Linear", "Bezier", "Hermite", "Bounce", "Elastic", "Back"}
                      : index.column() == 3
                          ? QStringList{"Free", "Aligned", "Vector", "Automatic", "AutoClamped"}
                          : QStringList{"In", "Out", "InOut"});
        return box;
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override {
        qobject_cast<QComboBox *>(editor)->setCurrentText(index.data().toString());
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override {
        model->setData(index, qobject_cast<QComboBox *>(editor)->currentText());
    }
};
class NativeCurvePlot : public QWidget {
  public:
    Scene snapshot;
    int channel = 0, component = 0;
    NativeCurvePlot(const Scene &s) : snapshot(s.motionOnlyCopy()) {
        setMinimumHeight(180);
    }
    double sample(double time) {
        const auto &c = (*snapshot.clips)[snapshot.sourceClip].channels[channel];
        snapshot.evaluateNativeSource(float(time * snapshot.fps));
        if (c.path == "translation")
            return double(snapshot.pose[c.node].t[component]);
        if (c.path == "scale")
            return double(snapshot.pose[c.node].s[component]);
        if (c.path == "weights")
            return double(snapshot.morphWeights(c.node)[component]);
        auto q = snapshot.pose[c.node].r;
        return double(component == 3 ? q.scalar() : q.vector()[component]);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(28, 31, 36));
        p.setRenderHint(QPainter::Antialiasing);
        const auto &c = (*snapshot.clips)[snapshot.sourceClip].channels[channel];
        double a = c.times.front(), b = std::max(a + .001, double(c.times.back())), low = INFINITY,
               high = -INFINITY;
        std::vector<double> values;
        for (int i = 0; i <= 500; ++i) {
            double v = sample(a + (b - a) * i / 500);
            values.push_back(v);
            low = std::min(low, v);
            high = std::max(high, v);
        }
        high = std::max(high, low + .001);
        QPainterPath path;
        for (int i = 0; i <= 500; ++i) {
            QPointF point(25. + (width() - 50.) * i / 500,
                          height() - 25. - (height() - 50.) * (values[i] - low) / (high - low));
            if (i)
                path.lineTo(point);
            else
                path.moveTo(point);
        }
        p.setPen(QPen(QColor(130, 190, 240), 2));
        p.drawPath(path);
        p.drawText(8, 15, "Native source (read-only) / " + c.interpolation + " / seconds");
    }
};
class CurveCanvas : public QWidget {
  public:
    Document *doc;
    QString id;
    int drag = -1, handle = 0;
    bool panning = false, boxing = false;
    QRectF box;
    QSet<int> selected;
    QSet<QString> visibleChannels;
    std::vector<CurveKey> clipboard;
    std::vector<CurveKey> dragStart;
    QPointF dragPress;
    QPointF panPoint;
    double first = 0, last = 3, low = -1, high = 1;
    explicit CurveCanvas(Document *d) : doc(d) {
        setMinimumSize(260, 190);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        connect(d, &Document::changed, this, QOverload<>::of(&QWidget::update));
    }
    ScalarCurve *curve() {
        for (auto &c : doc->scene.curves)
            if (c.id == id)
                return &c;
        return nullptr;
    }
    QPointF point(double t, double v) const {
        return {40 + (t - first) / (last - first) * std::max(1, width() - 55),
                height() - 25 - (v - low) / (high - low) * std::max(1, height() - 40)};
    }
    double time(double x) const {
        return first + (x - 40) / std::max(1, width() - 55) * (last - first);
    }
    double value(double y) const {
        return low + (height() - 25 - y) / std::max(1, height() - 40) * (high - low);
    }
    void fit() {
        first = double(doc->scene.first) / doc->scene.fps;
        last = std::max(first + .01, double(doc->scene.last) / doc->scene.fps);
        low = 0;
        high = 1;
        if (auto c = curve()) {
            low = high = c->keys.front().value;
            for (int i = 0; i <= 200; ++i) {
                double v = sampleCurve(*c, first + (last - first) * i / 200);
                low = std::min(low, v);
                high = std::max(high, v);
            }
            for (const auto &other : doc->scene.curves)
                if (other.path == c->path && visibleChannels.contains(other.id))
                    for (int i = 0; i <= 200; ++i) {
                        double v = sampleCurve(other, first + (last - first) * i / 200);
                        low = std::min(low, v);
                        high = std::max(high, v);
                    }
            double margin = std::max(.1, (high - low) * .15);
            low -= margin;
            high += margin;
        }
        update();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(28, 31, 36));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor(90, 95, 105));
        p.drawText(8, 16, "Author local channel / seconds");
        p.drawLine(point(first, 0), point(last, 0));
        auto c = curve();
        if (!c)
            return;
        int legend = 30, color = 0;
        for (const auto &other : doc->scene.curves)
            if (other.id != id && other.path == c->path && visibleChannels.contains(other.id)) {
                QPainterPath overlay;
                for (int i = 0; i <= width(); ++i) {
                    double t = first + (last - first) * i / std::max(1, width());
                    auto q = point(t, sampleCurve(other, t));
                    if (i)
                        overlay.lineTo(q);
                    else
                        overlay.moveTo(q);
                }
                auto tint = QColor::fromHsv((color++ * 71 + 30) % 360, 160, 210);
                p.setPen(QPen(tint, 1));
                p.drawPath(overlay);
                p.drawText(8, legend,
                           doc->scene.nodes[other.node].name + " / " + other.path + " " +
                               QString::number(other.component));
                legend += 15;
            }
        QPainterPath path;
        for (int i = 0; i <= width(); ++i) {
            double t = first + (last - first) * i / std::max(1, width());
            auto q = point(t, sampleCurve(*c, t));
            if (i)
                path.lineTo(q);
            else
                path.moveTo(q);
        }
        p.setPen(QPen(QColor(100, 205, 230), 2));
        p.drawPath(path);
        int keyIndex = 0;
        for (auto &k : c->keys) {
            p.setPen(QColor(130, 140, 150));
            auto a = point(k.time, c->path == "rotationQuaternion" ? sampleCurve(*c, k.time) : k.value);
            if (c->path != "rotationQuaternion" && (k.handles == "Free" || k.handles == "Aligned")) {
                auto b = point(k.time + k.outTime, k.value + k.outValue),
                     d = point(k.time + k.inTime, k.value + k.inValue);
                p.drawLine(a, b);
                p.drawLine(a, d);
                p.drawEllipse(b, 3, 3);
                p.drawEllipse(d, 3, 3);
            }
            p.setBrush(selected.contains(keyIndex++) ? QColor(255, 250, 220) : QColor(245, 194, 65));
            p.drawEllipse(a, 4, 4);
        }
        if (boxing) {
            p.setPen(QColor(100, 180, 240));
            p.setBrush(QColor(100, 180, 240, 40));
            p.drawRect(box.normalized());
        }
        p.setPen(QColor(230, 100, 90));
        p.drawLine(point(double(doc->frame) / doc->scene.fps, low),
                   point(double(doc->frame) / doc->scene.fps, high));
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::MiddleButton || e->button() == Qt::RightButton) {
            panning = true;
            panPoint = e->position();
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        auto c = curve();
        if (!c)
            return;
        for (int i = 0; i < int(c->keys.size()); ++i) {
            auto &k = c->keys[i];
            if (c->path != "rotationQuaternion" && (k.handles == "Free" || k.handles == "Aligned"))
                for (int side : {-1, 1})
                    if (QLineF(point(k.time + (side < 0 ? k.inTime : k.outTime),
                                     k.value + (side < 0 ? k.inValue : k.outValue)),
                               e->position())
                            .length() < 8) {
                        drag = i;
                        handle = side;
                        doc->begin();
                        return;
                    }
        }
        for (int i = 0; i < int(c->keys.size()); ++i)
            if (QLineF(point(c->keys[i].time, c->path == "rotationQuaternion"
                                                  ? sampleCurve(*c, c->keys[i].time)
                                                  : c->keys[i].value),
                       e->position())
                    .length() < 9) {
                if (e->modifiers() & Qt::ControlModifier) {
                    if (selected.contains(i))
                        selected.remove(i);
                    else
                        selected.insert(i);
                    update();
                    return;
                }
                if (!selected.contains(i)) {
                    selected.clear();
                    selected.insert(i);
                }
                drag = i;
                handle = 0;
                dragStart = c->keys;
                dragPress = e->position();
                doc->begin();
                return;
            }
        if (e->modifiers() & Qt::ShiftModifier) {
            boxing = true;
            box = QRectF(e->position(), e->position());
            if (!(e->modifiers() & Qt::ControlModifier))
                selected.clear();
            return;
        }
        doc->seek(std::clamp(int(std::round(time(e->position().x()) * doc->scene.fps)), doc->scene.first,
                             doc->scene.last));
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (boxing) {
            box.setBottomRight(e->position());
            update();
            return;
        }
        if (panning) {
            double dt = time(panPoint.x()) - time(e->position().x()),
                   dv = value(panPoint.y()) - value(e->position().y());
            first += dt;
            last += dt;
            low += dv;
            high += dv;
            panPoint = e->position();
            update();
            return;
        }
        if (drag < 0)
            return;
        auto c = curve();
        if (!c)
            return;
        auto &k = c->keys[drag];
        if (handle) {
            double dt = time(e->position().x()) - k.time, dv = value(e->position().y()) - k.value;
            if (handle < 0) {
                double maxTime = drag ? k.time - c->keys[drag - 1].time : 1e8;
                k.inTime = std::clamp(dt, -maxTime, -std::min(1e-6, maxTime * .01));
                k.inValue = dv;
                if (k.handles == "Aligned")
                    k.outValue = k.outTime * k.inValue / k.inTime;
            } else {
                double maxTime = drag + 1 < int(c->keys.size()) ? c->keys[drag + 1].time - k.time : 1e8;
                k.outTime = std::clamp(dt, std::min(1e-6, maxTime * .01), maxTime);
                k.outValue = dv;
                if (k.handles == "Aligned")
                    k.inValue = k.inTime * k.outValue / k.outTime;
            }
        } else {
            double dt = time(e->position().x()) - time(dragPress.x()),
                   dv = value(e->position().y()) - value(dragPress.y());
            double lower = -1e12, upper = 1e12;
            for (int i : selected)
                if (i < int(dragStart.size())) {
                    if (i > 0 && !selected.contains(i - 1))
                        lower = std::max(lower, std::nextafter(dragStart[i - 1].time, dragStart[i].time) -
                                                    dragStart[i].time);
                    if (i + 1 < int(dragStart.size()) && !selected.contains(i + 1))
                        upper = std::min(upper, std::nextafter(dragStart[i + 1].time, dragStart[i].time) -
                                                    dragStart[i].time);
                }
            dt = std::clamp(dt, lower, upper);
            for (int i : selected)
                if (i < int(c->keys.size())) {
                    c->keys[i].time = dragStart[i].time + dt;
                    c->keys[i].value = dragStart[i].value + dv;
                }
        }
        doc->scene.evaluate(doc->frame);
        emit doc->changed();
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (boxing) {
            boxing = false;
            if (auto c = curve())
                for (int i = 0; i < int(c->keys.size()); ++i)
                    if (box.normalized().contains(point(c->keys[i].time, c->keys[i].value)))
                        selected.insert(i);
            double a = time(box.left()), b = time(box.right());
            doc->selectTime(a, b);
            update();
            return;
        }
        panning = false;
        if (drag < 0)
            return;
        drag = -1;
        QString error;
        if (validateCurves(doc->scene, &error))
            doc->commit(handle ? "Drag curve tangent" : "Drag curve key", false);
        else {
            doc->cancel();
            emit doc->feedback(error);
        }
    }
    void keyPressEvent(QKeyEvent *e) override {
        auto ptr = curve();
        if (!ptr) {
            QWidget::keyPressEvent(e);
            return;
        }
        if (e->matches(QKeySequence::Copy)) {
            clipboard.clear();
            for (int i : selected)
                if (i < int(ptr->keys.size()))
                    clipboard.push_back(ptr->keys[i]);
            std::sort(clipboard.begin(), clipboard.end(), [](auto &a, auto &b) { return a.time < b.time; });
            return;
        }
        auto c = *ptr;
        bool changed = false;
        if (e->key() == Qt::Key_I) {
            double time = double(doc->frame) / doc->scene.fps;
            auto found = std::find_if(c.keys.begin(), c.keys.end(),
                                      [&](auto &key) { return std::abs(key.time - time) < 1e-10; });
            if (found == c.keys.end()) {
                CurveKey key;
                key.time = time;
                key.value = sampleCurve(c, time);
                if (c.path == "rotationQuaternion")
                    key.orientation = sampleOrientation(c, time);
                c.keys.push_back(key);
                std::sort(c.keys.begin(), c.keys.end(), [](auto &a, auto &b) { return a.time < b.time; });
                changed = true;
            }
        } else if (e->matches(QKeySequence::Paste) && !clipboard.empty()) {
            double dt = double(doc->frame) / doc->scene.fps - clipboard.front().time;
            for (auto key : clipboard) {
                key.time += dt;
                auto it = std::find_if(c.keys.begin(), c.keys.end(),
                                       [&](auto &k) { return std::abs(k.time - key.time) < 1e-10; });
                if (it == c.keys.end())
                    c.keys.push_back(key);
                else
                    *it = key;
            }
            std::sort(c.keys.begin(), c.keys.end(), [](auto &a, auto &b) { return a.time < b.time; });
            changed = true;
        } else if (e->key() == Qt::Key_Delete) {
            for (int i = int(c.keys.size()) - 1; i >= 0; --i)
                if (selected.contains(i))
                    c.keys.erase(c.keys.begin() + i);
            changed = true;
        } else if (e->key() == Qt::Key_A && (e->modifiers() & Qt::ControlModifier)) {
            for (int i = 0; i < int(c.keys.size()); ++i)
                selected.insert(i);
            update();
            return;
        }
        if (changed) {
            try {
                if (c.keys.empty())
                    EditService::execute(*doc, "animation_curves", {{"operation", "remove"}, {"id", c.id}},
                                         "Delete curve keys");
                else
                    EditService::execute(*doc, "animation_curves",
                                         {{"operation", "set"}, {"id", c.id}, {"curve", curvesJson({c})[0]}},
                                         "Edit curve keys");
                selected.clear();
                update();
            } catch (const std::exception &error) {
                emit doc->feedback(error.what());
            }
            return;
        }
        QWidget::keyPressEvent(e);
    }
    void wheelEvent(QWheelEvent *e) override {
        double f = e->angleDelta().y() > 0 ? .8 : 1.25, mid = time(e->position().x());
        first = mid + (first - mid) * f;
        last = mid + (last - mid) * f;
        update();
    }
};
void Window::showGraphEditor() {
    auto &d = document();
    QDialog dialog(this);
    dialog.setWindowTitle("Graph Editor — authored local channels");
    dialog.resize(1000, 690);
    auto available = screen()->availableGeometry();
    available.setSize(available.size().boundedTo(size()));
    dialog.resize(std::min(dialog.width(), available.width() - 30),
                  std::min(dialog.height(), available.height() - 40));
    auto outer = new QVBoxLayout(&dialog);
    auto tabs = new QTabWidget;
    outer->addWidget(tabs);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    tabs->addTab(scroll, "Authored channels");
    auto content = new QWidget;
    scroll->setWidget(content);
    auto layout = new QVBoxLayout(content);
    auto controls = new QHBoxLayout;
    layout->addLayout(controls);
    auto bones = new QComboBox;
    for (auto &n : d.scene.nodes)
        bones->addItem(n.name);
    bones->setCurrentIndex(std::max(0, d.selected));
    controls->addWidget(bones);
    auto scope = new QComboBox;
    scope->addItem("Source override", QString());
    for (auto &layer : d.scene.layers)
        scope->addItem("Offset: " + layer.name, layer.id);
    controls->addWidget(scope);
    auto paths = new QComboBox;
    paths->addItems({"position", "rotationEuler", "rotationQuaternion", "scale", "morph", "ikTarget",
                     "ikPole", "ikWeight"});
    controls->addWidget(paths);
    auto component = new QSpinBox;
    component->setRange(0, 2);
    controls->addWidget(component);
    auto componentRange = [=, &d] {
        int n = bones->currentIndex();
        int count = (paths->currentText() == "rotationQuaternion" || paths->currentText() == "ikWeight") ? 1
                    : paths->currentText() == "morph" && n >= 0 ? int(d.scene.morphWeights(n).size())
                                                                : 3;
        component->setRange(0, std::max(0, count - 1));
    };
    connect(paths, &QComboBox::currentIndexChanged, &dialog, componentRange);
    connect(bones, &QComboBox::currentIndexChanged, &dialog, componentRange);

    auto create = new QPushButton("Create / select channel");
    controls->addWidget(create);
    auto channels = new QComboBox;
    controls->addWidget(channels);
    auto visibility = new QListWidget;
    visibility->setMaximumHeight(80);
    layout->addWidget(visibility);
    visibility->setToolTip("Check channels to overlay with the active channel. Only matching units/path are "
                           "shown. The dropdown chooses the editable channel.");
    auto canvas = new CurveCanvas(&d);
    connect(visibility, &QListWidget::itemChanged, &dialog, [=](QListWidgetItem *item) {
        auto id = item->data(Qt::UserRole).toString();
        if (item->checkState() == Qt::Checked)
            canvas->visibleChannels.insert(id);
        else
            canvas->visibleChannels.remove(id);
        canvas->fit();
    });
    layout->addWidget(canvas, 1);
    auto keyPage = new QSpinBox;
    keyPage->setObjectName("curveKeyPage");
    keyPage->setPrefix("Key page ");
    keyPage->setMinimum(1);
    keyPage->setToolTip("512 editable rows per page; the curve and selection retain all keys.");
    layout->addWidget(keyPage);
    int keyOffset = 0;
    auto table = new QTableWidget;
    table->setObjectName("authoredCurveKeys");
    table->setColumnCount(12);
    for (int column : {2, 3, 8})
        table->setItemDelegateForColumn(column, new CurveEnumDelegate(table));
    table->setHorizontalHeaderLabels({"Seconds", "Value", "Interpolation", "Handles", "In seconds",
                                      "In value", "Out seconds", "Out value", "Ease", "Overshoot",
                                      "Amplitude", "Period"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->setMaximumHeight(240);
    layout->addWidget(table);
    auto footer = new QHBoxLayout;
    layout->addLayout(footer);
    auto add = new QPushButton("Add at playhead"), remove = new QPushButton("Delete selected keys"),
         fit = new QPushButton("Fit");
    footer->addWidget(add);
    footer->addWidget(remove);
    footer->addWidget(fit);
    auto quaternionEdit = new QPushButton("Quaternion / angular tangents");
    footer->addWidget(quaternionEdit);
    footer->addStretch();
    auto note = new QLabel("I: key; Shift-drag: box select; Ctrl+C/V: keys. Source TRS uses local values; "
                           "layer TRS uses additive offsets. IK target/pole: world coordinates; weight "
                           "clamps to [0,1]. Euler: YXZ degrees. Quaternion graph: computed angle.");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto refresh = [&] {
        QSignalBlocker blocker(table);
        table->setRowCount(0);
        auto c = canvas->curve();
        if (!c)
            return;
        {
            QSignalBlocker pageBlock(keyPage);
            keyPage->setMaximum(std::max(1, int((c->keys.size() + 511) / 512)));
        }
        keyOffset = (keyPage->value() - 1) * 512;
        for (int keyIndex = keyOffset; keyIndex < std::min(keyOffset + 512, int(c->keys.size()));
             ++keyIndex) {
            const auto &k = c->keys[keyIndex];
            int row = table->rowCount();
            table->insertRow(row);
            QStringList values{
                QString::number(k.time, 'g', 15),
                QString::number(c->path == "rotationQuaternion" ? sampleCurve(*c, k.time) : k.value, 'g', 15),
                k.interpolation,
                k.handles,
                QString::number(k.inTime),
                QString::number(k.inValue),
                QString::number(k.outTime),
                QString::number(k.outValue),
                k.ease,
                QString::number(k.overshoot),
                QString::number(k.amplitude),
                QString::number(k.period)};
            for (int i = 0; i < 12; ++i)
                table->setItem(row, i, new QTableWidgetItem(values[i]));
        }
    };
    connect(keyPage, &QSpinBox::valueChanged, &dialog, [&] { refresh(); });
    auto reload = [&] {
        QString id = canvas->id;
        QSignalBlocker block(channels);
        channels->clear();
        QSignalBlocker visibilityBlock(visibility);
        visibility->clear();
        for (auto &c : d.scene.curves) {
            auto item = new QListWidgetItem(QString("%1 / %2 %3 / %4")
                                                .arg(d.scene.nodes[c.node].name, c.path)
                                                .arg(c.component)
                                                .arg(c.layer.isEmpty() ? "source" : c.layer),
                                            visibility);
            item->setData(Qt::UserRole, c.id);
            item->setCheckState(canvas->visibleChannels.contains(c.id) ? Qt::Checked : Qt::Unchecked);
        }
        for (auto &c : d.scene.curves)
            channels->addItem(QString("%1 / %2 %3").arg(d.scene.nodes[c.node].name, c.path).arg(c.component),
                              c.id);
        int index = channels->findData(id);
        channels->setCurrentIndex(index < 0 ? 0 : index);
        canvas->id = channels->currentData().toString();
        refresh();
        canvas->fit();
    };
    auto apply = [&](ScalarCurve c) {
        try {
            EditService::execute(d, "animation_curves",
                                 {{"operation", "set"}, {"id", c.id}, {"curve", curvesJson({c})[0]}},
                                 "Edit curve");
            refresh();
            canvas->update();
        } catch (const std::exception &e) {
            QMessageBox::warning(&dialog, "Curve", QString::fromUtf8(e.what()));
            refresh();
        }
    };
    connect(channels, &QComboBox::currentIndexChanged, &dialog, [&] {
        canvas->id = channels->currentData().toString();
        refresh();
        canvas->fit();
    });
    connect(create, &QPushButton::clicked, &dialog, [&] {
        if (bones->currentIndex() < 0)
            return;
        for (auto &c : d.scene.curves)
            if (c.node == bones->currentIndex() && c.path == paths->currentText() &&
                c.component == component->value() && c.layer == scope->currentData().toString()) {
                canvas->id = c.id;
                reload();
                return;
            }
        ScalarCurve c;
        c.layer = scope->currentData().toString();
        c.node = bones->currentIndex();
        c.path = paths->currentText();
        c.component = component->value();
        c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        double value = 0;
        if (c.path == "scale")
            value = 1;
        else if (c.path == "position")
            value = d.scene.pose[c.node].t[c.component];
        if (c.path == "scale")
            value = d.scene.pose[c.node].s[c.component];
        if (c.path == "rotationEuler")
            value = d.scene.pose[c.node].r.toEulerAngles()[c.component];
        if (c.path == "morph" && c.component < int(d.scene.morphWeights(c.node).size()))
            value = d.scene.morphWeights(c.node)[c.component];
        if (!c.layer.isEmpty() && !c.path.startsWith("ik"))
            value = 0;
        if (c.path == "ikTarget")
            value = d.scene.pins.contains(c.node) ? d.scene.pins[c.node].target[c.component]
                                                  : d.scene.positions(d.scene.pose)[c.node][c.component];
        if (c.path == "ikPole")
            value = d.scene.pins.contains(c.node) ? d.scene.pins[c.node].pole[c.component]
                                                  : d.scene.defaultPole(c.node)[c.component];
        if (c.path == "ikWeight")
            value = d.scene.pins.contains(c.node) ? d.scene.pins[c.node].weight : 1;
        c.keys = {{double(d.frame) / d.scene.fps, value}};
        if (c.path == "rotationQuaternion")
            c.keys[0].orientation = c.layer.isEmpty() ? d.scene.pose[c.node].r : QQuaternion();
        apply(c);
        canvas->id = c.id;
        reload();
    });
    connect(table, &QTableWidget::cellChanged, &dialog, [&](int row, int col) {
        auto ptr = canvas->curve();
        if (!ptr || row + keyOffset >= int(ptr->keys.size()))
            return;
        auto c = *ptr;
        auto &k = c.keys[row + keyOffset];
        auto text = table->item(row, col)->text();
        bool ok = true;
        double v = text.toDouble(&ok);
        if (col != 2 && col != 3 && col != 8 && !ok) {
            refresh();
            return;
        }
        switch (col) {
        case 0:
            k.time = v;
            break;
        case 1:
            k.value = v;
            break;
        case 2:
            k.interpolation = text;
            break;
        case 3:
            k.handles = text;
            break;
        case 4:
            k.inTime = v;
            break;
        case 5:
            k.inValue = v;
            break;
        case 6:
            k.outTime = v;
            break;
        case 7:
            k.outValue = v;
            break;
        case 8:
            k.ease = text;
            break;
        case 9:
            k.overshoot = v;
            break;
        case 10:
            k.amplitude = v;
            break;
        case 11:
            k.period = v;
            break;
        }
        if (k.handles == "Aligned") {
            if ((col == 4 || col == 5) && k.inTime < 0)
                k.outValue = k.outTime * k.inValue / k.inTime;
            else if (k.outTime > 0)
                k.inValue = k.inTime * k.outValue / k.outTime;
        }
        std::sort(c.keys.begin(), c.keys.end(), [](auto &a, auto &b) { return a.time < b.time; });
        apply(c);
    });
    connect(add, &QPushButton::clicked, &dialog, [&] {
        auto ptr = canvas->curve();
        if (!ptr)
            return;
        auto c = *ptr;
        double t = double(d.frame) / d.scene.fps;
        for (auto &k : c.keys)
            if (std::abs(k.time - t) < 1e-8)
                return;
        CurveKey k;
        k.time = t;
        k.value = sampleCurve(c, t);
        if (c.path == "rotationQuaternion")
            k.orientation = sampleOrientation(c, t);
        c.keys.push_back(k);
        std::sort(c.keys.begin(), c.keys.end(), [](auto &a, auto &b) { return a.time < b.time; });
        apply(c);
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        auto ptr = canvas->curve();
        if (!ptr)
            return;
        auto c = *ptr;
        QSet<int> rows=canvas->selected;
        for (auto i : table->selectedItems())
            rows.insert(i->row() + keyOffset);
        for (int i = int(c.keys.size()) - 1; i >= 0; --i)
            if (rows.contains(i))
                c.keys.erase(c.keys.begin() + i);
        canvas->selected.clear();
        if (!c.keys.empty())
            apply(c);
        else {
            EditService::execute(d, "animation_curves", {{"operation", "remove"}, {"id", c.id}},
                                 "Delete curve");
            canvas->id.clear();
            reload();
        }
    });
    auto mode = new QComboBox;
    mode->addItems({"Step", "Linear", "Bezier", "Hermite", "Bounce", "Elastic", "Back"});
    auto bulk = new QPushButton("Interpolation for selected keys");
    auto bulkRow = new QHBoxLayout;
    bulkRow->addWidget(mode);
    bulkRow->addWidget(bulk);
    layout->addLayout(bulkRow);
    connect(bulk, &QPushButton::clicked, &dialog, [&] {
        auto ptr = canvas->curve();
        if (!ptr)
            return;
        auto c = *ptr;
        QSet<int> rows = canvas->selected;
        for (auto item : table->selectedItems())
            rows.insert(item->row() + keyOffset);
        for (int row : rows)
            if (row < int(c.keys.size()))
                c.keys[row].interpolation = mode->currentText();
        apply(c);
    });
    connect(quaternionEdit, &QPushButton::clicked, &dialog, [&] {
        auto ptr = canvas->curve();
        if (!ptr || ptr->path != "rotationQuaternion") {
            note->setText("Choose a rotationQuaternion channel first. The graph shows computed geodesic "
                          "angle, not quaternion components.");
            return;
        }
        int row = table->currentRow();
        if (row < 0 || row + keyOffset >= int(ptr->keys.size()))
            return;
        auto c = *ptr;
        auto &k = c.keys[row + keyOffset];
        QDialog edit(&dialog);
        edit.setWindowTitle("Quaternion XYZW / local angular velocity rad/s");
        auto form = new QFormLayout(&edit);
        std::array<QDoubleSpinBox *, 10> fields;
        for (int i = 0; i < 10; ++i) {
            auto field = new QDoubleSpinBox;
            field->setDecimals(7);
            field->setRange(i < 4 ? -1 : -1000, i < 4 ? 1 : 1000);
            field->setValue(i < 4   ? (i == 3 ? k.orientation.scalar() : k.orientation.vector()[i])
                            : i < 7 ? k.angularIn[i - 4]
                                    : k.angularOut[i - 7]);
            fields[i] = field;
            form->addRow(i < 4 ? QString("Quaternion %1").arg(QString("XYZW")[i])
                               : QString("%1 omega %2")
                                     .arg(i < 7 ? "Incoming" : "Outgoing")
                                     .arg(QString("XYZ")[(i - 4) % 3]),
                         field);
        }
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &edit, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &edit, &QDialog::reject);
        if (edit.exec() == QDialog::Accepted) {
            k.orientation =
                QQuaternion(fields[3]->value(), fields[0]->value(), fields[1]->value(), fields[2]->value());
            if (k.orientation.lengthSquared() < 1e-12) {
                note->setText("Zero quaternion rejected.");
                return;
            }
            k.orientation.normalize();
            for (int i = 0; i < 3; ++i) {
                k.angularIn[i] = fields[i + 4]->value();
                k.angularOut[i] = fields[i + 7]->value();
            }
            apply(c);
        }
    });
    connect(fit, &QPushButton::clicked, canvas, [&] { canvas->fit(); });
    connect(&d, &Document::changed, &dialog, [&] {
        if (!d.inTransaction())
            refresh();
    });
    reload();
    if (d.scene.clips && d.scene.sourceClip >= 0 && !(*d.scene.clips)[d.scene.sourceClip].channels.empty()) {
        auto page = new QWidget;
        auto box = new QVBoxLayout(page);
        auto bar = new QHBoxLayout;
        box->addLayout(bar);
        auto native = new QComboBox;
        auto axis = new QSpinBox;
        auto extract = new QPushButton("Copy to authored override");
        bar->addWidget(native, 1);
        bar->addWidget(axis);
        bar->addWidget(extract);
        const auto &nativeChannels = (*d.scene.clips)[d.scene.sourceClip].channels;
        for (auto &c : nativeChannels)
            native->addItem(d.scene.nodes[c.node].name + " / " + c.path + " / " + c.interpolation);
        auto plot = new NativeCurvePlot(d.scene);
        box->addWidget(plot, 1);
        auto keys = new QTableWidget;
        keys->setColumnCount(5);
        keys->setHorizontalHeaderLabels(
            {"Seconds", "Value", "Incoming derivative", "Outgoing derivative", "Native mode"});
        keys->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        box->addWidget(keys, 1);
        auto hint = new QLabel(
            "Native data stays intact. Scalar cubic channels copy as Hermite; cubic quaternion conversion "
            "explicitly bakes SLERP with 0.02 degree sampled tolerance. Table shows up to 2000 native keys.");
        hint->setWordWrap(true);
        box->addWidget(hint);
        auto updateNative = [=, &d] {
            auto &c = (*d.scene.clips)[d.scene.sourceClip].channels[native->currentIndex()];
            axis->setRange(0, c.components - 1);
            plot->channel = native->currentIndex();
            plot->component = axis->value();
            plot->update();
            keys->setRowCount(std::min(2000, int(c.times.size())));
            for (int row = 0; row < keys->rowCount(); ++row) {
                int index = c.interpolation == "CUBICSPLINE" ? row * 3 + 1 : row;
                QStringList cells{
                    QString::number(c.times[row], 'g', 10),
                    QString::number(c.values[index * c.components + axis->value()], 'g', 10),
                    c.interpolation == "CUBICSPLINE"
                        ? QString::number(c.values[row * 3 * c.components + axis->value()])
                        : QString(),
                    c.interpolation == "CUBICSPLINE"
                        ? QString::number(c.values[(row * 3 + 2) * c.components + axis->value()])
                        : QString(),
                    c.interpolation};
                for (int col = 0; col < 5; ++col) {
                    auto item = new QTableWidgetItem(cells[col]);
                    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                    keys->setItem(row, col, item);
                }
            }
        };
        connect(native, &QComboBox::currentIndexChanged, &dialog, updateNative);
        connect(axis, &QSpinBox::valueChanged, &dialog, updateNative);
        connect(keys, &QTableWidget::cellDoubleClicked, &dialog, [=, &d](int row, int) {
            d.seek(std::clamp(
                qRound((*d.scene.clips)[d.scene.sourceClip].channels[native->currentIndex()].times[row] *
                       d.scene.fps),
                d.scene.first, d.scene.last));
        });
        connect(extract, &QPushButton::clicked, &dialog, [=, &d, &dialog, &reload] {
            try {
                EditService::execute(d, "animation_curves",
                                     {{"operation", "extract_native"},
                                      {"channel", native->currentIndex()},
                                      {"component", axis->value()}},
                                     "Extract native channel");
                reload();
                tabs->setCurrentIndex(0);
            } catch (const std::exception &e) {
                QMessageBox::warning(&dialog, "Native channel", e.what());
            }
        });
        tabs->addTab(page, "Native source");
        updateNative();
    }
    dialog.exec();
    if (d.inTransaction())
        d.cancel();
    for (auto object : dialog.findChildren<QObject *>())
        QObject::disconnect(object, nullptr, &dialog, nullptr);
}
} // namespace trickster
