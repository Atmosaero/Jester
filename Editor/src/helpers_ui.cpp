#include "window.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <algorithm>
namespace trickster {
void Window::buildHelpers() {
    auto group = new QGroupBox("Joint limit", selectionLabel->parentWidget());
    auto row = new QHBoxLayout(group);
    limitEnabled = new QCheckBox("Limit from rest", group);
    limitEnabled->setObjectName("limitEnabled");
    limitEnabled->setToolTip("Constrain the total rotation angle relative to this joint's rest rotation. "
                             "This is a cone limit, not an anatomical hinge.");
    limitAngle = new QDoubleSpinBox(group);
    limitAngle->setObjectName("limitAngle");
    limitAngle->setRange(0, 180);
    limitAngle->setSuffix("°");
    limitAngle->setDecimals(1);
    limitAngle->setValue(150);
    limitAngle->setKeyboardTracking(false);
    row->addWidget(limitEnabled);
    row->addWidget(limitAngle);
    auto layout = qobject_cast<QVBoxLayout *>(selectionLabel->parentWidget()->layout());
    layout->insertWidget(3, group);
    auto updateLimit = [this] {
        if (refreshing || document().selected < 0)
            return;
        bool enabled = limitEnabled->isChecked();
        float angle = float(limitAngle->value());
        if (document().scene.jointLimits.contains(document().selected) && enabled)
            return;
        if (enabled == document().scene.rotationLimits.contains(document().selected) &&
            (!enabled ||
             std::abs(document().scene.rotationLimits.value(document().selected) - angle) < .001f))
            return;
        document().edit("Change joint rotation limit", [&](Scene &s) {
            if (enabled)
                s.rotationLimits[document().selected] = angle;
            else
                s.rotationLimits.remove(document().selected);
            s.jointLimits.remove(document().selected);
            s.enforceLimits();
            s.solvePins();
        });
    };
    connect(limitEnabled, &QCheckBox::toggled, this, updateLimit);
    connect(limitAngle, &QDoubleSpinBox::editingFinished, this, updateLimit);
    auto pose = menuBar()->addMenu("&Pose");
    pose->setObjectName("poseMenu");
    auto mirror = pose->addAction("Flip entire pose across X");
    mirror->setShortcut(QKeySequence("Ctrl+M"));
    mirror->setToolTip("Mirror rest-relative offsets across X; pairs Left/Right, .L/.R or _L/_R. Best on "
                       "symmetric humanoid rigs.");
    connect(mirror, &QAction::triggered, this, [this] {
        QStringList missing;
        for (int n = 0; n < int(document().scene.nodes.size()); n++)
            if (document().scene.sideNamed(n) && document().scene.mirrorPartner(n) < 0)
                missing << document().scene.nodes[n].name;
        document().edit("Flip entire pose", [](Scene &s) { s.mirrorPose(); });
        if (!missing.isEmpty())
            report("Unpaired side bones were left unchanged: " + missing.join(", ") +
                       ". Review Character profile > Mirror pairs.",
                   true);
    });
    symmetryAction = pose->addAction("Symmetric editing across X");
    symmetryAction->setObjectName("symmetryAction");
    symmetryAction->setToolTip("Mirror the edited FK bone / IK chain to its assigned partner, with one Undo. "
                               "Shift+M. Review pairs in Character profile.");
    symmetryAction->setCheckable(true);
    symmetryAction->setShortcut(QKeySequence("Shift+M"));
    connect(symmetryAction, &QAction::toggled, this, [this](bool value) { document().setSymmetry(value); });
    auto presets = pose->addMenu("Pose presets");
    auto rest = presets->addAction("Rest pose");
    connect(rest, &QAction::triggered, this, [this] { document().resetPose(); });
    auto crouch = presets->addAction("Crouch · named humanoid");
    connect(crouch, &QAction::triggered, this, [this] {
        int hips = document().scene.profile.roles.value("pelvis", -1);
        for (int i = document().scene.profile.configured ? int(document().scene.nodes.size()) : 0;
             i < int(document().scene.nodes.size()); ++i)
            if (document().scene.nodes[i].name.contains("hips", Qt::CaseInsensitive) ||
                document().scene.nodes[i].name.contains("pelvis", Qt::CaseInsensitive)) {
                hips = i;
                break;
            }
        if (hips < 0) {
            report("Crouch preset needs a Hips or Pelvis joint. You can pose this rig manually.");
            return;
        }
        document().edit("Crouch preset", [&](Scene &s) {
            s.pose = s.restPose();
            s.pins.clear();
            auto p = s.positions(s.pose);
            for (int i = 0; i < int(s.nodes.size()); ++i)
                if (s.isEffector(i) && s.isFoot(i))
                    s.pins[i] = {p[i], s.defaultPole(i), 1};
            s.translateWorld(hips, {0, -s.height() * .12f, 0});
        });
    });
    pose->addSeparator();
    assistAction = pose->addAction("Body assists unreachable IK goals · heuristic");
    assistAction->setObjectName("bodyAssistAction");
    assistAction->setCheckable(true);
    assistAction->setToolTip("For named humanoids, move the hips slightly and turn the chest toward a "
                             "distant goal. Existing pins are solved again. This is not neural posing.");
    connect(assistAction, &QAction::toggled, this, [this](bool on) {
        if (!refreshing && on != document().scene.bodyAssist)
            document().edit("Toggle body assistance", [on](Scene &s) { s.bodyAssist = on; }, false);
    });
    auto contacts = pose->addAction("Foot contact intervals…");
    contacts->setObjectName("contactsAction");
    connect(contacts, &QAction::triggered, this, &Window::contactDialog);
    auto balance = pose->addAction("Show center of mass / support");
    balance->setCheckable(true);
    balance->setObjectName("balanceAction");
    connect(balance, &QAction::toggled, this, [this](bool on) {
        viewport->showBalance = on;
        viewport->update();
    });
    pose->addSeparator();
    auto bake = pose->addAction("Bake corrections to project copy…");
    bake->setObjectName("bakeCopyAction");
    connect(bake, &QAction::triggered, this, [this] { bakedCopyDialog(false); });
    auto arc = pose->addAction("Ballistic COM arc to copy · experimental…");
    arc->setObjectName("arcCopyAction");
    arc->setVisible(false);
    connect(arc, &QAction::triggered, this, [this] { bakedCopyDialog(true); });
}
void Window::contactDialog() {
    commitPendingEditor();
    document().play(false);
    QDialog dialog(this);
    dialog.setObjectName("contactDialog");
    dialog.setWindowTitle("Jester · Contacts");
    dialog.resize(600, 540);
    auto layout = new QVBoxLayout(&dialog);
    auto hint = new QLabel("Choose an IK effector and interval. Capture its current target or place it on "
                           "Ground Y. Limits stay active.",
                           &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto list = new QListWidget(&dialog);
    list->setObjectName("contactList");
    list->setMaximumHeight(110);
    layout->addWidget(list);
    auto form = new QFormLayout;
    auto effector = new QComboBox(&dialog);
    effector->setObjectName("contactEffector");
    for (int n = 0; n < int(document().scene.nodes.size()); ++n)
        if (document().scene.isEffector(n))
            effector->addItem(document().scene.nodes[n].name, n);
    effector->setCurrentIndex(effector->findData(document().selected));
    form->addRow("Effector", effector);
    auto range = new QWidget(&dialog);
    auto rangeLayout = new QHBoxLayout(range);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    auto begin = new QSpinBox(range), end = new QSpinBox(range);
    begin->setObjectName("contactFirst");
    end->setObjectName("contactLast");
    for (auto spin : {begin, end}) {
        spin->setRange(document().scene.first, document().scene.last);
        rangeLayout->addWidget(spin);
    }
    begin->setValue(document().frame);
    end->setValue(document().scene.last);
    form->addRow("Interval", range);
    std::array<QDoubleSpinBox *, 3> targetFields{}, orientationFields{};
    auto vectorFields = [&](auto &fields, QString prefix) {
        auto row = new QWidget(&dialog);
        auto l = new QHBoxLayout(row);
        l->setContentsMargins(0, 0, 0, 0);
        for (int i = 0; i < 3; ++i) {
            auto spin = new QDoubleSpinBox(row);
            spin->setRange(-100000, 100000);
            spin->setDecimals(4);
            spin->setKeyboardTracking(false);
            spin->setPrefix(QString("XYZ")[i] + QString(" "));
            spin->setObjectName(prefix + QString::number(i));
            l->addWidget(spin);
            fields[i] = spin;
        }
        return row;
    };
    form->addRow("World position", vectorFields(targetFields, "contactTarget"));
    auto orient = new QCheckBox("Hold orientation", &dialog);
    orient->setObjectName("contactOrientation");
    orient->setChecked(true);
    form->addRow(orient);
    form->addRow("World Euler °", vectorFields(orientationFields, "contactRotation"));
    auto fades = new QWidget(&dialog);
    auto fl = new QHBoxLayout(fades);
    fl->setContentsMargins(0, 0, 0, 0);
    auto fadeIn = new QSpinBox(fades), fadeOut = new QSpinBox(fades);
    fadeIn->setObjectName("contactFadeIn");
    fadeOut->setObjectName("contactFadeOut");
    for (auto f : {fadeIn, fadeOut}) {
        f->setRange(0, 100000);
        f->setValue(3);
        fl->addWidget(f);
    }
    form->addRow("Fade in / out", fades);
    layout->addLayout(form);
    auto feedback = new QLabel(&dialog);
    feedback->setObjectName("contactFeedback");
    feedback->setWordWrap(true);
    layout->addWidget(feedback);
    auto captureRow = new QHBoxLayout;
    auto capture = new QPushButton("Capture current", &dialog),
         ground = new QPushButton("On ground", &dialog);
    capture->setObjectName("contactCaptureButton");
    ground->setObjectName("contactGroundButton");
    captureRow->addWidget(capture);
    captureRow->addWidget(ground);
    layout->addLayout(captureRow);
    auto captureTarget = [&] {
        int n = (effector->currentIndex() < 0 ? -1 : effector->currentData().toInt());
        if (n < 0)
            return;
        auto p = document().scene.positions(document().scene.pose)[n];
        auto q = document().scene.globalRotation(n, document().scene.pose).toEulerAngles();
        for (int i = 0; i < 3; ++i) {
            targetFields[i]->setValue(p[i]);
            orientationFields[i]->setValue(q[i]);
        }
    };
    connect(capture, &QPushButton::clicked, &dialog, captureTarget);
    connect(ground, &QPushButton::clicked, &dialog, [&] {
        captureTarget();
        int node = effector->currentData().toInt();
        auto target = groundContactTarget(document().scene, node,
                                          document().scene.positions(document().scene.pose)[node]);
        targetFields[1]->setValue(target.y());
    });
    auto fill = [&] {
        int selected = list->currentRow();
        list->clear();
        for (const auto &c : document().scene.contacts)
            list->addItem(
                QString("%1 · %2–%3").arg(document().scene.nodes[c.node].name).arg(c.first).arg(c.last));
        if (selected >= 0 && selected < list->count())
            list->setCurrentRow(selected);
    };
    connect(list, &QListWidget::currentRowChanged, &dialog, [&](int index) {
        if (index < 0 || index >= int(document().scene.contacts.size()))
            return;
        auto c = document().scene.contacts[index];
        effector->setCurrentIndex(effector->findData(c.node));
        begin->setValue(c.first);
        end->setValue(c.last);
        fadeIn->setValue(c.fadeIn);
        fadeOut->setValue(c.fadeOut);
        orient->setChecked(c.orient);
        auto e = c.orientation.toEulerAngles();
        for (int i = 0; i < 3; ++i) {
            targetFields[i]->setValue(c.target[i]);
            orientationFields[i]->setValue(e[i]);
        }
    });
    auto row = new QHBoxLayout;
    auto add = new QPushButton("Add", &dialog), update = new QPushButton("Update", &dialog),
         remove = new QPushButton("Remove", &dialog), check = new QPushButton("Check", &dialog);
    add->setObjectName("contactAddButton");
    update->setObjectName("contactUpdateButton");
    remove->setObjectName("contactRemoveButton");
    check->setObjectName("diagnoseContactsButton");
    for (auto button : {add, update, remove, check})
        row->addWidget(button);
    layout->addLayout(row);
    auto write = [&](bool replace) {
        if (effector->currentIndex() < 0) {
            feedback->setText("Assign an IK chain in Profile before adding a contact.");
            return;
        }
        if (replace && list->currentRow() < 0) {
            feedback->setText("Choose an existing contact to update.");
            return;
        }
        QJsonArray p, q;
        for (int i = 0; i < 3; ++i) {
            p.append(targetFields[i]->value());
            q.append(orientationFields[i]->value());
        }
        if (applyOperation("contact",
                           {{"operation", replace ? "set" : "add"},
                            {"index", list->currentRow()},
                            {"node", effector->currentData().toInt()},
                            {"first", begin->value()},
                            {"last", end->value()},
                            {"target", p},
                            {"orientation", q},
                            {"orient", orient->isChecked()},
                            {"fadeIn", fadeIn->value()},
                            {"fadeOut", fadeOut->value()}},
                           replace ? "Update contact" : "Add contact")) {
            fill();
            feedback->setText("Saved. Check the interval for reach and limit conflicts.");
        } else
            feedback->setText(statusBar()->currentMessage());
    };
    connect(add, &QPushButton::clicked, &dialog, [&] { write(false); });
    connect(update, &QPushButton::clicked, &dialog, [&] { write(true); });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() >= 0) {
            applyOperation("contact", {{"operation", "remove"}, {"index", list->currentRow()}},
                           "Remove contact");
            fill();
        }
    });
    connect(check, &QPushButton::clicked, &dialog, [&] {
        auto report = document().scene.diagnoseContacts();
        feedback->setText(QString("%1 half-frame samples · maximum %2 mm / %3°")
                              .arg(report.samples)
                              .arg(report.position * 1000, 0, 'f', 2)
                              .arg(report.angle, 0, 'f', 2) +
                          (report.warnings.empty() ? QString() : "\n" + report.warnings.join("\n")));
    });
    auto close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(close);
    captureTarget();
    fill();
    dialog.exec();
}
void Window::bakedCopyDialog(bool arc) {
    document().play(false);
    int from = document().scene.first, to = document().scene.last;
    float height = document().scene.height() * .2f;
    if (qint64(to - from + 1) * qint64(document().scene.nodes.size()) > 5000000) {
        report("Clip is too large to bake in memory. Shorten the range.", true);
        return;
    }
    if (arc) {
        QDialog dialog(this);
        dialog.setObjectName("arcDialog");
        dialog.setWindowTitle("Jester · Experimental COM arc");
        auto layout = new QVBoxLayout(&dialog);
        auto label = new QLabel(
            "Writes a separate project copy. The estimated COM follows a parabola between the chosen "
            "endpoint COM positions. Foot contacts and pins are baked and removed in the copy. No collision, "
            "landing or dynamic balance solver is applied. Units follow the imported rig.",
            &dialog);
        label->setWordWrap(true);
        label->setMinimumWidth(420);
        layout->addWidget(label);
        auto form = new QFormLayout;
        auto a = new QSpinBox(&dialog), b = new QSpinBox(&dialog);
        a->setRange(from, to);
        b->setRange(from, to);
        a->setValue(from);
        b->setValue(to);
        auto apex = new QDoubleSpinBox(&dialog);
        apex->setRange(0, 10000);
        apex->setDecimals(3);
        apex->setValue(height);
        form->addRow("Takeoff frame", a);
        form->addRow("Landing frame", b);
        form->addRow("Apex above endpoint line", apex);
        layout->addLayout(form);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        from = a->value();
        to = b->value();
        height = float(apex->value());
        if (from >= to) {
            report("Landing must be after takeoff.", true);
            return;
        }
    }
    QString path =
        QFileDialog::getSaveFileName(this, arc ? "Save experimental arc copy" : "Save baked project copy",
                                     lastDirectory + (arc ? "/root-arc.trickster" : "/baked.trickster"),
                                     "Trickster project (*.trickster)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".trickster", Qt::CaseInsensitive))
        path += ".trickster";
    if (!document().path.isEmpty() &&
        QFileInfo(path).absoluteFilePath() == QFileInfo(document().path).absoluteFilePath()) {
        report("Choose a different filename for the copy.", true);
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Scene result = arc ? document().scene.ballisticCopy(from, to, height) : document().scene.bakedCopy();
    QString error;
    bool ok = saveProject(path, result, error);
    QApplication::restoreOverrideCursor();
    if (ok)
        report("Saved separate project copy: " + path);
    else
        report(error, true);
}
} // namespace trickster
