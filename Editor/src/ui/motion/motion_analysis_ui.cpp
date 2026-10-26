#include "ui/widgets/localized_ui.h"
#include "ui/motion/analysis_views.h"
#include "core/analysis/motion_analysis.h"
#include "core/animation/retarget.h"
#include "ui/window/window.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QSaveFile>
#include <QScreen>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
namespace trickster {
void Window::showMotionAnalysis() {
    auto &d = document();
    QDialog dialog(this);
    bindUiText(&dialog,"windowTitle","Motion Paths / numerical analysis");
    dialog.resize(880, 660);
    auto available = screen()->availableGeometry();
    available.setSize(available.size().boundedTo(size()));
    dialog.resize(std::min(dialog.width(), available.width() - 30),
                  std::min(dialog.height(), available.height() - 40));
    auto layout = new QVBoxLayout(&dialog);
    auto settingsWidget = new QWidget;
    auto settingsLayout = new QVBoxLayout(settingsWidget);
    auto settingsScroll = new QScrollArea;
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setWidget(settingsWidget);
    settingsScroll->setMaximumHeight(250);
    layout->addWidget(settingsScroll, 1);
    auto list = new QListWidget;
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setMaximumHeight(150);
    for (int n = 0; n < int(d.scene.nodes.size()); ++n) {
        auto item = new QListWidgetItem(d.scene.nodes[n].name, list);
        item->setData(Qt::UserRole, n);
        if (d.selection().contains(n))
            item->setSelected(true);
    }
    settingsLayout->addWidget(list);
    auto form = new QFormLayout;
    settingsLayout->addLayout(form);
    auto start = new QDoubleSpinBox, end = new QDoubleSpinBox, step = new QDoubleSpinBox;
    for (auto v : {start, end, step}) {
        v->setDecimals(6);
        v->setRange(-1e6, 1e6);
    }
    step->setRange(.000001, 1e6);
    start->setValue(double(d.scene.first) / d.scene.fps);
    end->setValue(double(d.scene.last) / d.scene.fps);
    step->setValue(1. / d.scene.fps);
    if (d.timeSelection) {
        start->setValue(d.timeSelection->first);
        end->setValue(d.timeSelection->second);
    }
    addTranslatedRow(form,"Start seconds", start);
    addTranslatedRow(form,"End seconds", end);
    addTranslatedRow(form,"Step seconds", step);
    auto source = new QComboBox;
    addTranslatedItems(source,{"evaluated", "original"});
    addTranslatedRow(form,"Source", source);
    auto pointKind = new QComboBox;
    addTranslatedItems(pointKind,{"origin", "end", "offset"});
    addTranslatedRow(form,"Sample point", pointKind);
    auto reference = new QComboBox;
    reference->setObjectName("trajectorySpace");
    addTranslatedItems(reference,{"world", "parent", "root"});
    addTranslatedRow(form,"Reference", reference);
    auto units = new QComboBox;
    units->setObjectName("trajectoryUnits");
    addTranslatedItems(units,{"scene", "metres"});
    addTranslatedRow(form,"Linear units", units);
    auto pathLabels = translatedWidget<QCheckBox>("Event and frame labels");
    pathLabels->setChecked(viewport->motionPathLabels);
    auto speedColors = translatedWidget<QCheckBox>("Speed colours (0 to 3 scene units/s)");
    auto autoUpdate = translatedWidget<QCheckBox>("Update paths automatically after motion edits");
    form->addRow(pathLabels);
    form->addRow(speedColors);
    form->addRow(autoUpdate);
    auto pathWindow = new QDoubleSpinBox;
    pathWindow->setRange(-1, 10000);
    pathWindow->setValue(-1);
    bindUiText(pathWindow,"specialValueText","Whole calculated interval");
    addTranslatedRow(form,"Past/future window seconds", pathWindow);
    auto offsetLayout = new QHBoxLayout;
    std::array<QDoubleSpinBox *, 3> offsets;
    for (int c = 0; c < 3; ++c) {
        auto v = new QDoubleSpinBox;
        v->setDecimals(6);
        v->setRange(-1e5, 1e5);
        offsets[c] = v;
        offsetLayout->addWidget(v);
    }
    addTranslatedRow(form,"Local offset (scene units)", offsetLayout);
    auto mode = new QComboBox;
    mode->setObjectName("motionAnalysisMode");
    addTranslatedItems(mode,{"Motion paths", "Compare open documents", "Validate motion"});
    addTranslatedRow(form,"Tool", mode);
    auto other = new QComboBox;
    for (int i = 0; i < int(workspace.documents.size()); ++i)
        other->addItem(workspace.documents[i]->scene.name, i);
    addTranslatedRow(form,"Compare against", other);
    auto alignment = new QComboBox;
    addTranslatedItems(alignment,{"time", "phase", "event"});
    addTranslatedRow(form,"Alignment", alignment);
    auto leftEvent = new QComboBox, rightEvent = new QComboBox;
    for (auto value : d.scene.annotations) {
        auto event = value.toObject();
        leftEvent->addItem(event["name"].toString(), event["id"].toString());
    }
    auto updateEvents = [&] {
        rightEvent->clear();
        if (other->currentIndex() < 0)
            return;
        for (auto value : workspace.documents[other->currentData().toInt()]->scene.annotations) {
            auto event = value.toObject();
            rightEvent->addItem(event["name"].toString(), event["id"].toString());
        }
    };
    connect(other, &QComboBox::currentIndexChanged, &dialog, updateEvents);
    updateEvents();
    addTranslatedRow(form,"This event", leftEvent);
    addTranslatedRow(form,"Other event", rightEvent);
    auto timeOffset = new QDoubleSpinBox;
    timeOffset->setRange(-1e5, 1e5);
    timeOffset->setDecimals(6);
    addTranslatedRow(form,"Other time offset seconds", timeOffset);
    QJsonArray comparisonMapping;
    auto mappingButton = translatedWidget<QPushButton>("Review bone pairs / anatomical calibration");
    form->addRow(mappingButton);
    connect(other, &QComboBox::currentIndexChanged, &dialog, [&] { comparisonMapping = {}; });
    connect(mappingButton, &QPushButton::clicked, &dialog, [&] {
        if (other->currentIndex() < 0)
            return;
        const auto &target = workspace.documents[other->currentData().toInt()]->scene;
        QDialog review(&dialog);
        bindUiText(&review,"windowTitle","Comparison pairs / calibrated rest-local axes");
        review.resize(std::min(1000, available.width() - 40), std::min(600, available.height() - 50));
        auto box = new QVBoxLayout(&review);
        auto pairs = new QTableWidget;
        pairs->setColumnCount(16);
        bindHeaders(pairs,{"Role", "This bone", "Other bone", "Explicit axes", "Axis L X",
                                          "L Y", "L Z", "Axis R X", "R Y", "R Z", "Rest L X", "L Y", "L Z",
                                          "Rest R X", "R Y", "R Z"});
        box->addWidget(pairs, 1);
        auto appendPair = [&](QJsonObject pair) {
            int row = pairs->rowCount();
            pairs->insertRow(row);
            pairs->setItem(row, 0, new QTableWidgetItem(pair["role"].toString("custom")));
            auto left = new QComboBox, right = new QComboBox;
            for (auto &n : d.scene.nodes)
                left->addItem(n.name);
            for (auto &n : target.nodes)
                right->addItem(n.name);
            left->setCurrentIndex(pair["left"].toInt(-1));
            right->setCurrentIndex(pair["right"].toInt(-1));
            pairs->setCellWidget(row, 1, left);
            pairs->setCellWidget(row, 2, right);
            auto explicitAxes = new QCheckBox;
            explicitAxes->setChecked(pair.contains("axisLeft"));
            pairs->setCellWidget(row, 3, explicitAxes);
            for (int col = 4; col < 16; ++col) {
                auto value = new QDoubleSpinBox;
                value->setRange(-360, 360);
                value->setDecimals(5);
                int side = col >= 10 ? (col - 10) / 3 : (col - 4) / 3,
                    c = (col >= 10 ? col - 10 : col - 4) % 3;
                auto values = pair[col >= 10 ? (side ? "referenceRight" : "referenceLeft")
                                             : (side ? "axisRight" : "axisLeft")]
                                  .toArray();
                int n = side ? right->currentIndex() : left->currentIndex();
                auto &scene = side ? target : d.scene;
                QVector3D fallback =
                    col >= 10 && n >= 0 ? scene.nodes[n].rest.r.toEulerAngles() : QVector3D(1, 0, 0);
                value->setValue(c < values.size() ? values[c].toDouble() : fallback[c]);
                pairs->setCellWidget(row, col, value);
            }
        };
        if (comparisonMapping.empty())
            for (auto value : suggestRetarget(d.scene, target)["mapping"].toArray()) {
                auto pair = value.toObject();
                if (pair["source"].toInt(-1) >= 0 && pair["target"].toInt(-1) >= 0)
                    appendPair({{"role", pair["role"]}, {"left", pair["source"]}, {"right", pair["target"]}});
            }
        else
            for (auto pair : comparisonMapping)
                appendPair(pair.toObject());
        auto controls = new QHBoxLayout;
        box->addLayout(controls);
        auto addPair = translatedWidget<QPushButton>("Add pair"), removePair = translatedWidget<QPushButton>("Remove pair");
        controls->addWidget(addPair);
        controls->addWidget(removePair);
        connect(addPair, &QPushButton::clicked, &review, [&] { appendPair({}); });
        connect(removePair, &QPushButton::clicked, &review, [&] {
            if (pairs->currentRow() >= 0)
                pairs->removeRow(pairs->currentRow());
        });
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        box->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &review, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &review, &QDialog::reject);
        if (review.exec() == QDialog::Accepted) {
            comparisonMapping = {};
            for (int row = 0; row < pairs->rowCount(); ++row) {
                int l = qobject_cast<QComboBox *>(pairs->cellWidget(row, 1))->currentIndex(),
                    r = qobject_cast<QComboBox *>(pairs->cellWidget(row, 2))->currentIndex();
                if (l < 0 || r < 0)
                    continue;
                QJsonObject pair{{"role", pairs->item(row, 0)->text()}, {"left", l}, {"right", r}};
                if (qobject_cast<QCheckBox *>(pairs->cellWidget(row, 3))->isChecked())
                    for (int group = 0; group < 4; ++group) {
                        QJsonArray values;
                        for (int c = 0; c < 3; ++c)
                            values.append(
                                qobject_cast<QDoubleSpinBox *>(pairs->cellWidget(row, 4 + group * 3 + c))
                                    ->value());
                        pair[QStringList{"axisLeft", "axisRight", "referenceLeft", "referenceRight"}[group]] =
                            values;
                    }
                comparisonMapping.append(pair);
            }
        }
    });
    auto normalize = translatedWidget<QCheckBox>("Normalize root position/orientation and height");
    form->addRow(normalize);
    auto relative = translatedWidget<QCheckBox>("Compare rest-relative rotations");
    relative->setChecked(true);
    form->addRow(relative);
    auto lag = translatedWidget<QCheckBox>("Estimate phase lag (sampled grid search)");
    form->addRow(lag);
    auto surface = translatedWidget<QCheckBox>("Check explicit mesh pair surfaces");
    surface->setObjectName("motionSurfaceCheck");
    form->addRow(surface);
    auto selfSurface = translatedWidget<QCheckBox>("Check self-intersection / nested components");
    form->addRow(selfSurface);
    auto adaptive = translatedWidget<QCheckBox>("Refine time near rapid motion / suspected intervals");
    adaptive->setChecked(true);
    form->addRow(adaptive);
    auto meshA = new QComboBox, meshB = new QComboBox;
    for (int i = 0; i < int(d.scene.meshes.size()); ++i) {
        meshA->addItem(uiText("Mesh %1").arg(i), i);
        meshB->addItem(uiText("Mesh %1").arg(i), i);
    }
    if (meshB->count() > 1)
        meshB->setCurrentIndex(1);
    addTranslatedRow(form,"Surface A", meshA);
    addTranslatedRow(form,"Surface B", meshB);
    auto updateSettings = [&] {
        bool trajectory = mode->currentIndex() == 0, comparison = mode->currentIndex() == 1,
             validation = mode->currentIndex() == 2;
        list->setVisible(trajectory);
        for (QWidget *widget : std::array<QWidget *, 8>{source, pointKind, reference, units, pathLabels,
                                                        speedColors, autoUpdate, pathWindow})
            form->setRowVisible(widget, trajectory);
        form->setRowVisible(offsetLayout, trajectory);
        for (QWidget *widget :
             std::array<QWidget *, 7>{other, alignment, normalize, relative, lag, timeOffset, mappingButton})
            form->setRowVisible(widget, comparison);
        form->setRowVisible(leftEvent, comparison && choiceText(alignment) == "event");
        form->setRowVisible(rightEvent, comparison && choiceText(alignment) == "event");
        for (QWidget *widget : std::array<QWidget *, 3>{surface, selfSurface, adaptive})
            form->setRowVisible(widget, validation);
        form->setRowVisible(meshA, validation && surface->isChecked());
        form->setRowVisible(meshB, validation && surface->isChecked());
    };
    connect(mode, &QComboBox::currentIndexChanged, &dialog, updateSettings);
    connect(alignment, &QComboBox::currentIndexChanged, &dialog, updateSettings);
    connect(surface, &QCheckBox::toggled, &dialog, updateSettings);
    updateSettings();
    auto buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    auto calculate = translatedWidget<QPushButton>("Calculate / update paths"), clear = translatedWidget<QPushButton>("Clear paths"),
         save = translatedWidget<QPushButton>("Export JSON");
    buttons->addWidget(calculate);
    buttons->addWidget(clear);
    buttons->addWidget(save);
    auto csv = translatedWidget<QPushButton>("Export CSV");
    buttons->addWidget(csv);
    auto cancel = translatedWidget<QPushButton>("Cancel");
    buttons->addWidget(cancel);
    auto progress = new QProgressBar;
    progress->setRange(0, 100);
    layout->addWidget(progress);
    auto status = translatedWidget<QLabel>("World coordinates; scene units. Paths are sampled hints, not a continuous collision proof.");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto results = new QTabWidget;
    layout->addWidget(results, 2);
    auto table = new QTableView;
    auto model = new AnalysisTableModel(table);
    table->setModel(model);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    addTranslatedTab(results,table,"Numbers / issues");
    auto problemText=[](const QJsonObject &issue) {
        return uiText("%1 \u2013 %2 s / %3 / %4 / %5")
            .arg(issue["firstSeconds"].toDouble(),0,'g',5).arg(issue["lastSeconds"].toDouble(),0,'g',5)
            .arg(ownedValueLabel(issue["kind"].toString()),ownedValueLabel(issue["status"].toString()),issue["problemId"].toString());
    };
    auto problemPage = new QWidget;
    auto problemLayout = new QVBoxLayout(problemPage);
    auto problemFilter = new QLineEdit;
    bindUiText(problemFilter,"placeholderText","Filter problem type, status or ID");
    auto problems = new QListWidget;
    problems->setObjectName("motionProblems");
    auto navigation = new QHBoxLayout;
    auto previousProblem = translatedWidget<QPushButton>("Previous"), nextProblem = translatedWidget<QPushButton>("Next");
    auto transparent = translatedWidget<QCheckBox>("Transparent mesh");
    auto volumes = translatedWidget<QCheckBox>("Approximate proxy volumes");
    navigation->addWidget(previousProblem);
    navigation->addWidget(nextProblem);
    navigation->addWidget(transparent);
    navigation->addWidget(volumes);
    problemLayout->addWidget(problemFilter);
    problemLayout->addWidget(problems, 1);
    problemLayout->addLayout(navigation);
    addTranslatedTab(results,problemPage,"Problems / intervals");
    connect(&LocalizationService::instance(),&LocalizationService::languageChanged,&dialog,[problems,problemText] {
        const QSignalBlocker blocker(problems);
        for(int i=0;i<problems->count();++i) {
            auto *item=problems->item(i);const auto issue=item->data(Qt::UserRole).toJsonObject();
            item->setText(problemText(issue));item->setToolTip(translatedDiagnostic(issue["method"].toString())+"\n"+translatedDiagnostic(issue["uncertainty"].toString()));
        }
    });
    connect(problemFilter, &QLineEdit::textChanged, &dialog, [=](QString text) {
        for (int i = 0; i < problems->count(); ++i)
            problems->item(i)->setHidden(!problems->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    auto navigate = [=](int delta) {
        for (int row = problems->currentRow() + delta; row >= 0 && row < problems->count(); row += delta)
            if (!problems->item(row)->isHidden()) {
                problems->setCurrentRow(row);
                break;
            }
    };
    connect(previousProblem, &QPushButton::clicked, &dialog, [=] { navigate(-1); });
    connect(nextProblem, &QPushButton::clicked, &dialog, [=] { navigate(1); });
    connect(problems, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *item) {
        if (!item)
            return;
        auto issue = item->data(Qt::UserRole).toJsonObject();
        viewport->selectedMotionProblem = issue;
        viewport->diagnosticOverlay = true;
        const double seconds=issue["peakSeconds"].isDouble()
                                 ? issue["peakSeconds"].toDouble() : issue["firstSeconds"].toDouble();
        d.seek(std::clamp(qRound(seconds*d.scene.fps),d.scene.first,d.scene.last));
        QSet<int> nodes;
        for (auto value : issue["nodes"].toArray())
            nodes.insert(value.toInt());
        if (!nodes.empty())
            d.setSelection(nodes);
        viewport->update();
    });
    connect(transparent, &QCheckBox::toggled, &dialog, [&](bool value) {
        viewport->diagnosticTransparent = value;
        viewport->update();
    });
    connect(volumes, &QCheckBox::toggled, &dialog, [&](bool value) {
        viewport->showPhysicsModel = value;
        viewport->update();
    });
    auto updatePathStyle = [&] {
        viewport->motionPathLabels = pathLabels->isChecked();
        viewport->motionPathSpeedColors = speedColors->isChecked();
        viewport->motionPathPastSeconds = viewport->motionPathFutureSeconds = pathWindow->value();
        viewport->update();
    };
    connect(pathLabels, &QCheckBox::toggled, &dialog, updatePathStyle);
    connect(speedColors, &QCheckBox::toggled, &dialog, updatePathStyle);
    connect(pathWindow, &QDoubleSpinBox::valueChanged, &dialog, updatePathStyle);
    auto plotPage = new QWidget;
    auto plotLayout = new QVBoxLayout(plotPage);
    auto plotControls = new QHBoxLayout;
    plotLayout->addLayout(plotControls);
    auto metric = new QComboBox;
    addTranslatedItems(metric,{"speed", "position", "velocity", "acceleration", "angularVelocity", "positionError",
                      "orientationErrorDegrees", "linearVelocityError", "angularVelocityError",
                      "swingErrorDegrees", "twistErrorDegrees"});
    auto component = new QSpinBox;
    component->setRange(0, 2);
    auto plotBone = new QComboBox;
    for (int n = 0; n < int(d.scene.nodes.size()); ++n)
        plotBone->addItem(d.scene.nodes[n].name, n);
    plotControls->addWidget(metric);
    plotControls->addWidget(component);
    plotControls->addWidget(plotBone);
    auto plot = new ComputedMotionPlot;
    plotLayout->addWidget(plot, 1);
    addTranslatedTab(results,plotPage,"Computed graph (read-only)");
    plot->seek = [&](double seconds) {
        d.seek(std::clamp(qRound(seconds * d.scene.fps), d.scene.first, d.scene.last));
    };
    auto updateMetric = [&] {
        plot->setMetric(choiceText(metric), component->value(), plotBone->currentData().toInt());
    };
    connect(metric, &QComboBox::currentIndexChanged, &dialog, updateMetric);
    connect(component, &QSpinBox::valueChanged, &dialog, updateMetric);
    connect(plotBone, &QComboBox::currentIndexChanged, &dialog, updateMetric);
    QJsonArray rows;
    QString jobId;
    quint64 resultRevision = 0;
    QJsonObject completed;
    QTimer poll;
    auto showResult = [&] {
        completed["samples"] = rows;
        model->setReport(completed);
        plot->setReport(completed);
        updateMetric();
        viewport->motionPaths.clear();
        viewport->motionPathBreaks.clear();
        viewport->motionPathSpeeds.clear();
        bool comparison = completed.contains("alignment"), validation = completed.contains("surfaceEnabled");
        if (validation) {
            d.motionProblems = completed["problems"].toArray();
            d.analysisMotionRevision = resultRevision;
            d.analysisMetadataRevision = d.metadataRevision;
            ++d.analysisRevision;
            problems->clear();
            for (auto value : d.motionProblems) {
                auto issue = value.toObject();
                auto item = new QListWidgetItem(problemText(issue),problems);
                item->setData(Qt::UserRole, issue);
                item->setToolTip(translatedDiagnostic(issue["method"].toString()) + "\n" + translatedDiagnostic(issue["uncertainty"].toString()));
            }
            timeline->update();
        }
        if (!comparison && !validation) {
            QString space = completed["space"].toString();
            float scale = completed["positionUnits"] == "metres" ? 1.f / d.scene.physics.metersPerUnit : 1.f;
            double anchorSeconds =
                rows.empty() ? double(d.frame) / d.scene.fps : rows[0].toObject()["seconds"].toDouble();
            auto snapshot = d.scene.motionOnlyCopy();
            snapshot.compareOriginal = completed["source"] == "original";
            snapshot.evaluate(float(anchorSeconds * d.scene.fps));
            auto anchorFrames = snapshot.globals(snapshot.pose);
            int root = snapshot.profile.roles.value("root", 0);
            viewport->motionPathReferenceLabel =
                space == "world" ? "world"
                                 : space + " anchored at " + QString::number(anchorSeconds, 'g', 5) + " s";
            completed["pathDisplayReference"] = viewport->motionPathReferenceLabel;
            auto displayPoint = [&](int node, const QJsonArray &p) {
                QVector3D point(float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble()));
                point *= scale;
                int reference = space == "root" ? root : space == "parent" ? snapshot.nodes[node].parent : -1;
                return reference >= 0 ? anchorFrames[reference].map(point) : point;
            };
            for (auto value : rows) {
                auto row = value.toObject();
                for (auto entry : row["bones"].toArray()) {
                    auto bone = entry.toObject();
                    if (bone["derivativeStatus"] == "undefined-at-discontinuity")
                        viewport->motionPathBreaks[bone["node"].toInt()].insert(row["seconds"].toDouble());
                }
            }
            int bones = rows.empty() ? 1 : std::max(1, int(rows[0].toObject()["bones"].toArray().size()));
            int perBone = std::max(2, std::min(2000, 8000 / bones));
            int stride = std::max(1, int((rows.size() + perBone - 1) / perBone));
            for (int i = 0; i < rows.size(); ++i)
                if (i % stride == 0 || i + 1 == rows.size()) {
                    auto r = rows[i].toObject();
                    for (auto value : r["bones"].toArray()) {
                        auto b = value.toObject();
                        auto p = b["position"].toArray();
                        viewport->motionPathSpeeds[b["node"].toInt()][r["seconds"].toDouble()] =
                            b["speed"].toDouble() * scale;
                        if (b["derivativeStatus"] == "undefined-at-discontinuity")
                            viewport->motionPathBreaks[b["node"].toInt()].insert(r["seconds"].toDouble());
                        viewport->motionPaths[b["node"].toInt()].push_back(
                            {r["seconds"].toDouble(), displayPoint(b["node"].toInt(), p)});
                    }
                }
        }
        viewport->motionPathRevision = resultRevision;
        viewport->update();
        bindUiTextFactory(status,"text",[completed,comparison,validation,count=rows.size(),issueCount=model->rowCount()] {return comparison ? uiText("%1 samples; RMS %2 %3; orientation %4 degrees; %5")
                                         .arg(count)
                                         .arg(completed["rmsPosition"].toDouble())
                                         .arg(ownedValueLabel(completed["positionUnits"].toString()))
                                         .arg(completed["rmsOrientationDegrees"].toDouble())
                                         .arg(ownedValueLabel(completed["status"].toString()))
                        : validation
                            ? uiText("%1 sampled instants / %2 issues. Fixed sampling can miss "
                                      "events; double-click to inspect.")
                                  .arg(count)
                                  .arg(issueCount)
                            : uiText("%1 samples; %2; %3 / seconds / rad/s. Path display is capped "
                                      "near 2000 points per bone / 8000 total; numeric results are retained.")
                                  .arg(count)
                                  .arg(ownedValueLabel(completed["status"].toString()))
                                  .arg(ownedValueLabel(completed["positionUnits"].toString()));});
    };
    connect(&poll, &QTimer::timeout, &dialog, [&] {
        analysisJobs.poll();
        auto state = analysisJobs.state(jobId);
        progress->setValue(state["progress"].toInt());
        auto stateName = state["status"].toString();
        if (stateName == "running")
            return;
        poll.stop();
        calculate->setEnabled(true);
        if (stateName != "ready") {
            status->setText(ownedValueLabel(stateName) + ": " + translatedDiagnostic(state["error"].toString()));
            return;
        }
        try {
            rows = {};
            int offset = 0;
            do {
                auto result = analysisJobs.result(jobId, offset, 2000);
                completed = result;
                for (auto row : result["samples"].toArray())
                    rows.append(row);
                offset = result["nextResultOffset"].toInt(-1);
            } while (offset >= 0);
            completed["samples"] = rows;
            if (completed.contains("surfaceEnabled")) {
                QJsonArray allProblems;
                int problemOffset = 0;
                do {
                    auto page = analysisJobs.result(jobId, problemOffset, 2000);
                    for (auto issue : page["problems"].toArray())
                        allProblems.append(issue);
                    problemOffset = page["nextProblemOffset"].toInt(-1);
                } while (problemOffset >= 0);
                completed["problems"] = allProblems;
            }
            resultRevision = state["motionRevision"].toInteger();
            showResult();
        } catch (const std::exception &e) {
            status->setText(e.what());
        }
    });
    connect(calculate, &QPushButton::clicked, &dialog, [&] {
        try {
            QJsonArray nodes;
            for (auto item : list->selectedItems())
                nodes.append(item->data(Qt::UserRole).toInt());
            QJsonObject request{{"nodes", nodes},
                                {"start", start->value()},
                                {"end", end->value()},
                                {"step", step->value()},
                                {"source", choiceText(source)},
                                {"alignment", choiceText(alignment)},
                                {"normalizeRoot", normalize->isChecked()},
                                {"normalizeHeight", normalize->isChecked()}};
            request["restRelative"] = relative->isChecked();
            request["leftEvent"] = leftEvent->currentData().toString();
            request["rightEvent"] = rightEvent->currentData().toString();
            request["offsetSeconds"] = timeOffset->value();
            if (!comparisonMapping.empty())
                request["mapping"] = comparisonMapping;
            if (mode->currentIndex() == 1 && lag->isChecked())
                request["operation"] = "lag";
            request["pointKind"] = choiceText(pointKind);
            request["space"] = choiceText(reference);
            request["units"] = choiceText(units);
            request["point"] = QJsonArray{offsets[0]->value(), offsets[1]->value(), offsets[2]->value()};
            request["surface"] = surface->isChecked() || selfSurface->isChecked();
            request["selfIntersections"] = selfSurface->isChecked();
            request["adaptive"] = adaptive->isChecked();
            if (surface->isChecked())
                request["meshPairs"] = QJsonArray{
                    QJsonValue(QJsonArray{meshA->currentData().toInt(), meshB->currentData().toInt()})};
            bool comparison = mode->currentIndex() == 1;
            Document *second = comparison ? workspace.documents[other->currentData().toInt()].get() : nullptr;
            auto state = analysisJobs.start(d,
                                            comparison                  ? "compare"
                                            : mode->currentIndex() == 2 ? "validate"
                                                                        : "trajectory",
                                            request, second);
            jobId = state["id"].toString();
            calculate->setEnabled(false);
            bindUiText(status,"text","Calculating an immutable motion snapshot...");
            poll.start(50);
        } catch (const std::exception &e) {
            showUiMessage(QMessageBox::Warning,&dialog,[&] {return uiText("Motion analysis");},[&] {return translatedDiagnostic(translatedDiagnostic(QString::fromUtf8(e.what())));});
        }
    });
    connect(cancel, &QPushButton::clicked, &dialog, [&] {
        if (!jobId.isEmpty())
            analysisJobs.cancel(jobId);
    });
    connect(clear, &QPushButton::clicked, &dialog, [&] {
        d.motionProblems = {};
        ++d.analysisRevision;
        problems->clear();
        viewport->diagnosticOverlay = false;
        viewport->diagnosticTransparent = false;
        viewport->selectedMotionProblem = {};
        viewport->motionPaths.clear();
        viewport->update();
        rows = {};
        model->setReport({});
        plot->setReport({});
    });
    connect(save, &QPushButton::clicked, &dialog, [&] {
        auto path = QFileDialog::getSaveFileName(&dialog, uiText("Export sampled trajectory"), {}, uiText("JSON (*.json)"));
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            completed["samples"] = rows;
            file.write(QJsonDocument(completed).toJson());
            file.commit();
        }
    });
    connect(table, &QTableView::doubleClicked, &dialog, [&](QModelIndex index) {
        d.seek(std::clamp(qRound(model->seconds(index.row()) * d.scene.fps), d.scene.first, d.scene.last));
        int n = model->node(index.row());
        if (n >= 0)
            d.select(n);
    });
    connect(csv, &QPushButton::clicked, &dialog, [&] {
        if (completed.contains("alignment") || completed.contains("surfaceEnabled")) {
            bindUiText(status,"text","Use JSON for mapped comparison data.");
            return;
        }
        auto path = QFileDialog::getSaveFileName(&dialog, uiText("Export trajectory CSV"), {}, uiText("CSV (*.csv)"));
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            status->setText(file.errorString());
            return;
        }
        file.write("seconds,node,x,y,z,vx,vy,vz,ax,ay,az,wx,wy,wz,speed,derivative_status\n");
        for (auto value : rows) {
            auto r = value.toObject();
            for (auto bone : r["bones"].toArray()) {
                auto b = bone.toObject();
                QStringList cells{QString::number(r["seconds"].toDouble(), 'g', 16),
                                  QString::number(b["node"].toInt())};
                for (auto key : {"position", "velocity", "acceleration", "angularVelocity"}) {
                    auto v = b[key].toArray();
                    for (int c = 0; c < 3; ++c)
                        cells.append(v.size() > c ? QString::number(v[c].toDouble(), 'g', 16) : QString());
                }
                cells.append(b["speed"].isNull() ? QString()
                                                 : QString::number(b["speed"].toDouble(), 'g', 16));
                cells.append(b["derivativeStatus"].toString());
                file.write((cells.join(',') + '\n').toUtf8());
            }
        }
        if (!file.commit())
            status->setText(file.errorString());
    });
    QTimer autoTimer;
    autoTimer.setSingleShot(true);
    autoTimer.setInterval(350);
    connect(&d, &Document::changed, &dialog, [&] {
        if (autoUpdate->isChecked() && mode->currentIndex() == 0 && resultRevision &&
            resultRevision != d.motionRevision && calculate->isEnabled())
            autoTimer.start();
    });
    connect(&autoTimer, &QTimer::timeout, &dialog, [&] {
        if (calculate->isEnabled())
            calculate->click();
    });
    dialog.exec();
    if (!jobId.isEmpty() && poll.isActive())
        analysisJobs.cancel(jobId);
    for (auto object : dialog.findChildren<QObject *>())
        QObject::disconnect(object, nullptr, &dialog, nullptr);
}
} // namespace trickster
