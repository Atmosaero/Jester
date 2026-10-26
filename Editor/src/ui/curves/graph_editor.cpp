#include "ui/widgets/localized_ui.h"
#include "ui/curves/curve_canvas.h"
#include "ui/curves/native_curve_plot.h"
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
    QString displayText(const QVariant &value,const QLocale &) const override {return uiText(value.toString());}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &index) const override {
        auto box = new QComboBox(parent);
        addTranslatedItems(box,index.column() == 2
                          ? QStringList{"Step", "Linear", "Bezier", "Hermite", "Bounce", "Elastic", "Back"}
                      : index.column() == 3
                          ? QStringList{"Free", "Aligned", "Vector", "Automatic", "AutoClamped"}
                          : QStringList{"In", "Out", "InOut"});
        return box;
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override {
        setChoiceText(qobject_cast<QComboBox *>(editor),index.data(Qt::EditRole).toString());
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override {
        model->setData(index,choiceText(qobject_cast<QComboBox *>(editor)));
    }
};


void Window::showGraphEditor() {
    auto &d = document();
    QDialog dialog(this);
    bindUiText(&dialog,"windowTitle","Graph Editor — authored local channels");
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
    addTranslatedTab(tabs,scroll,"Authored channels");
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
    addTranslatedItem(scope,"Source override", QString());
    for (auto &layer : d.scene.layers)
        scope->addItem("Offset: " + layer.name, layer.id);
    controls->addWidget(scope);
    auto paths = new QComboBox;
    addTranslatedItems(paths,{"position", "rotationEuler", "rotationQuaternion", "scale", "morph", "ikTarget",
                     "ikPole", "ikWeight"});
    controls->addWidget(paths);
    auto component = new QSpinBox;
    component->setRange(0, 2);
    controls->addWidget(component);
    auto componentRange = [=, &d] {
        int n = bones->currentIndex();
        int count = (choiceText(paths) == "rotationQuaternion" || choiceText(paths) == "ikWeight") ? 1
                    : choiceText(paths) == "morph" && n >= 0 ? int(d.scene.morphWeights(n).size())
                                                                : 3;
        component->setRange(0, std::max(0, count - 1));
    };
    connect(paths, &QComboBox::currentIndexChanged, &dialog, componentRange);
    connect(bones, &QComboBox::currentIndexChanged, &dialog, componentRange);

    auto create = translatedWidget<QPushButton>("Create / select channel");
    controls->addWidget(create);
    auto channels = new QComboBox;
    controls->addWidget(channels);
    auto visibility = new QListWidget;
    visibility->setMaximumHeight(80);
    layout->addWidget(visibility);
    bindUiText(visibility,"toolTip","Check channels to overlay with the active channel. Only matching units/path are "
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
    bindUiText(keyPage,"prefix","Key page ");
    keyPage->setMinimum(1);
    bindUiText(keyPage,"toolTip","512 editable rows per page; the curve and selection retain all keys.");
    layout->addWidget(keyPage);
    int keyOffset = 0;
    auto table = new QTableWidget;
    table->setObjectName("authoredCurveKeys");
    table->setColumnCount(12);
    for (int column : {2, 3, 8})
        table->setItemDelegateForColumn(column, new CurveEnumDelegate(table));
    bindHeaders(table,{"Seconds", "Value", "Interpolation", "Handles", "In seconds",
                                      "In value", "Out seconds", "Out value", "Ease", "Overshoot",
                                      "Amplitude", "Period"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->setMaximumHeight(240);
    layout->addWidget(table);
    auto footer = new QHBoxLayout;
    layout->addLayout(footer);
    auto add = translatedWidget<QPushButton>("Add at playhead"), remove = translatedWidget<QPushButton>("Delete selected keys"),
         fit = translatedWidget<QPushButton>("Fit");
    footer->addWidget(add);
    footer->addWidget(remove);
    footer->addWidget(fit);
    auto quaternionEdit = translatedWidget<QPushButton>("Quaternion / angular tangents");
    footer->addWidget(quaternionEdit);
    footer->addStretch();
    auto note = translatedWidget<QLabel>("I: key; Shift-drag: box select; Ctrl+C/V: keys. Source TRS uses local values; "
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
            auto item = new QListWidgetItem(uiText("%1 / %2 %3 / %4")
                                                .arg(d.scene.nodes[c.node].name, c.path)
                                                .arg(c.component)
                                                .arg(c.layer.isEmpty() ? "source" : c.layer),
                                            visibility);
            item->setData(Qt::UserRole, c.id);
            item->setCheckState(canvas->visibleChannels.contains(c.id) ? Qt::Checked : Qt::Unchecked);
        }
        for (auto &c : d.scene.curves)
            channels->addItem(uiText("%1 / %2 %3").arg(d.scene.nodes[c.node].name, c.path).arg(c.component),
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
            showUiMessage(QMessageBox::Warning,&dialog,[&] {return uiText("Curve");},[&] {return translatedDiagnostic(translatedDiagnostic(QString::fromUtf8(e.what())));});
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
            if (c.node == bones->currentIndex() && c.path == choiceText(paths) &&
                c.component == component->value() && c.layer == scope->currentData().toString()) {
                canvas->id = c.id;
                reload();
                return;
            }
        ScalarCurve c;
        c.layer = scope->currentData().toString();
        c.node = bones->currentIndex();
        c.path = choiceText(paths);
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
    addTranslatedItems(mode,{"Step", "Linear", "Bezier", "Hermite", "Bounce", "Elastic", "Back"});
    auto bulk = translatedWidget<QPushButton>("Interpolation for selected keys");
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
                c.keys[row].interpolation = choiceText(mode);
        apply(c);
    });
    connect(quaternionEdit, &QPushButton::clicked, &dialog, [&] {
        auto ptr = canvas->curve();
        if (!ptr || ptr->path != "rotationQuaternion") {
            bindUiText(note,"text","Choose a rotationQuaternion channel first. The graph shows computed geodesic "
                          "angle, not quaternion components.");
            return;
        }
        int row = table->currentRow();
        if (row < 0 || row + keyOffset >= int(ptr->keys.size()))
            return;
        auto c = *ptr;
        auto &k = c.keys[row + keyOffset];
        QDialog edit(&dialog);
        bindUiText(&edit,"windowTitle","Quaternion XYZW / local angular velocity rad/s");
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
            form->addRow(i < 4 ? uiText("Quaternion %1").arg(QString("XYZW")[i])
                               : uiText("%1 omega %2")
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
                bindUiText(note,"text","Zero quaternion rejected.");
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
        auto extract = translatedWidget<QPushButton>("Copy to authored override");
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
        auto hint = translatedWidget<QLabel>("Native data stays intact. Scalar cubic channels copy as Hermite; cubic quaternion conversion "
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
                showUiMessage(QMessageBox::Warning,&dialog,[&] {return uiText("Native channel");},[&] {return translatedDiagnostic(translatedDiagnostic(QString::fromUtf8(e.what())));});
            }
        });
        addTranslatedTab(tabs,page,"Native source");
        updateNative();
    }
    dialog.exec();
    if (d.inTransaction())
        d.cancel();
    for (auto object : dialog.findChildren<QObject *>())
        QObject::disconnect(object, nullptr, &dialog, nullptr);
}
} // namespace trickster
