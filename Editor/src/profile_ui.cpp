#include "edit_service.h"
#include "window.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
namespace trickster {
void Window::jointLimitDialog() {
    commitPendingEditor();
    document().play(false);
    int node = document().selected;
    if (node < 0) {
        report("Select a joint first.");
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("jointLimitDialog");
    dialog.setWindowTitle("Jester · " + document().scene.nodes[node].name);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    auto type = new QComboBox;
    type->setObjectName("jointLimitKind");
    type->addItems({"Off", "Hinge", "Swing / twist"});
    const auto &scene = document().scene;
    const auto &limits = scene.physics.hasJointModel ? scene.physics.modelJointLimits : scene.jointLimits;
    auto limit = limits.value(node);
    type->setCurrentIndex(limits.contains(node) ? (limit.hinge ? 1 : 2) : 0);
    form->addRow("Constraint", type);
    std::array<QDoubleSpinBox *, 3> axis;
    auto row = new QWidget;
    auto axes = new QHBoxLayout(row);
    axes->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 3; ++i) {
        axis[i] = new QDoubleSpinBox;
        axis[i]->setRange(-1, 1);
        axis[i]->setDecimals(3);
        axis[i]->setValue(limit.axis[i]);
        axis[i]->setObjectName("jointLimitAxis" + QString::number(i));
        axes->addWidget(axis[i]);
    }
    form->addRow("Local rest axis", row);
    auto field = [&](QString name, float value) {
        auto box = new QDoubleSpinBox;
        box->setObjectName(name);
        box->setRange(-180, 180);
        box->setSuffix("°");
        box->setValue(value);
        return box;
    };
    auto min = field("jointLimitMin", limit.minimum), max = field("jointLimitMax", limit.maximum),
         swing = field("jointLimitSwing", limit.swing);
    swing->setMinimum(0);
    form->addRow("Minimum twist / bend", min);
    form->addRow("Maximum twist / bend", max);
    form->addRow("Maximum swing", swing);
    auto note = new QLabel(
        "Axis and angles are relative to the joint's rest orientation. Hinge removes "
        "swing; MuJoCo preserves hinge endpoints and reports its three-axis swing approximation separately.");
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addLayout(form);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            EditService::execute(document(), "pose_limits",
                                 {{"node", node},
                                  {"enabled", type->currentIndex() != 0},
                                  {"kind", type->currentIndex() == 1 ? "hinge" : "swing_twist"},
                                  {"axis", QJsonArray{axis[0]->value(), axis[1]->value(), axis[2]->value()}},
                                  {"swing", swing->value()},
                                  {"minimum", min->value()},
                                  {"maximum", max->value()}},
                                 "Joint limit");
        } catch (const std::exception &error) {
            QMessageBox::information(&dialog, "Joint limit", QString::fromUtf8(error.what()));
            return;
        }
        dialog.accept();
    });
    dialog.exec();
}
void Window::mirrorPairsDialog() {
    QTimer::singleShot(0, this, [this] {
        auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "characterProfileDialog")
            return;
        auto pairs = dialog->findChild<QTreeWidget *>("profileMirrorPairs");
        if (!pairs)
            return;
        int selected = document().selected;
        QTreeWidgetItem *match = nullptr;
        for (int row = 0; row < pairs->topLevelItemCount(); row++)
            for (int col = 0; col < 2; col++)
                if (auto combo = qobject_cast<QComboBox *>(pairs->itemWidget(pairs->topLevelItem(row), col)))
                    if (combo->currentData().toInt() == selected)
                        match = pairs->topLevelItem(row);
        if (!match && selected >= 0) {
            if (auto add = dialog->findChild<QPushButton *>("addMirrorPairButton"))
                add->click();
            match = pairs->topLevelItem(pairs->topLevelItemCount() - 1);
            if (match) {
                auto left = qobject_cast<QComboBox *>(pairs->itemWidget(match, 0)),
                     right = qobject_cast<QComboBox *>(pairs->itemWidget(match, 1));
                if (left)
                    left->setCurrentIndex(left->findData(selected));
                int suggestion = document().scene.suggestedProfile().mirrors.value(selected, -1);
                if (right)
                    right->setCurrentIndex(right->findData(suggestion));
            }
        }
        if (match) {
            pairs->setCurrentItem(match);
            pairs->scrollToItem(match);
        }
        for (auto parent = pairs->parentWidget(); parent; parent = parent->parentWidget())
            if (auto scroll = qobject_cast<QScrollArea *>(parent))
                scroll->ensureWidgetVisible(pairs);
    });
    characterProfileDialog();
}
void Window::characterProfileDialog() {
    commitPendingEditor();
    document().play(false);
    if (document().scene.nodes.empty()) {
        report("Open a character first.");
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("characterProfileDialog");
    dialog.setWindowTitle("Jester · Character profile");
    dialog.resize(520, 650);
    auto layout = new QVBoxLayout(&dialog);
    auto scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto content = new QWidget;
    auto form = new QFormLayout(content);
    auto profile =
        document().scene.profile.configured ? document().scene.profile : document().scene.suggestedProfile();
    auto status = new QLabel("Review roles, IK poles and explicit mirror pairs. Missing roles disable only "
                             "the related tools; assignments use saved node IDs and rest axes.");
    status->setWordWrap(true);
    layout->addWidget(status);
    QMap<QString, QComboBox *> roles;
    auto combo = [&](int node) {
        auto box = new QComboBox;
        box->addItem("Unassigned", -1);
        for (int n = 0; n < int(document().scene.nodes.size()); ++n)
            box->addItem(document().scene.nodes[n].name, n);
        box->setCurrentIndex(node + 1);
        return box;
    };
    QStringList names{"root", "pelvis", "spine", "chest", "neck", "head"};
    for (auto side : {QString("left"), QString("right")})
        for (auto part : {"Shoulder", "UpperArm", "ForeArm", "Hand", "UpperLeg", "Leg", "Foot", "Toe"})
            names << side + part;
    for (auto name : names) {
        auto box = combo(profile.roles.value(name, -1));
        box->setObjectName("profile_" + name);
        roles[name] = box;
        form->addRow(name, box);
    }
    QMap<QString, std::array<QDoubleSpinBox *, 3>> poles;
    for (auto side : {QString("left"), QString("right")})
        for (auto limb : {QString("Arm"), QString("Leg")}) {
            auto key = side + limb;
            auto group = new QGroupBox(key + " pole direction");
            auto row = new QHBoxLayout(group);
            int end = profile.roles.value(side + (limb == "Arm" ? "Hand" : "Foot"), -1);
            auto direction =
                profile.chains.value(end, IKChain{-1, -1, -1, {0, 0, limb == "Arm" ? -1.f : 1.f}}).pole;
            for (int axis = 0; axis < 3; ++axis) {
                auto box = new QDoubleSpinBox;
                box->setObjectName("profile_" + key + "Pole" + QString::number(axis));
                box->setRange(-1, 1);
                box->setDecimals(3);
                box->setSingleStep(.1);
                box->setValue(direction[axis]);
                poles[key][axis] = box;
                row->addWidget(box);
            }
            form->addRow(group);
        }
    auto mirrors = new QTreeWidget;
    mirrors->setObjectName("profileMirrorPairs");
    mirrors->setColumnCount(2);
    mirrors->setHeaderLabels({"Joint", "Mirror partner"});
    mirrors->setRootIsDecorated(false);
    mirrors->header()->setSectionResizeMode(QHeaderView::Stretch);
    mirrors->setMinimumHeight(130);
    mirrors->setMaximumHeight(190);
    auto pairRow = [&](int left, int right) {
        auto item = new QTreeWidgetItem(mirrors);
        mirrors->setItemWidget(item, 0, combo(left));
        mirrors->setItemWidget(item, 1, combo(right));
    };
    for (auto it = profile.mirrors.cbegin(); it != profile.mirrors.cend(); ++it)
        if (it.key() < it.value())
            pairRow(it.key(), it.value());
    form->addRow("Mirror pairs", mirrors);
    auto pairButtons = new QWidget;
    auto pairLayout = new QHBoxLayout(pairButtons);
    pairLayout->setContentsMargins(0, 0, 0, 0);
    auto addPair = new QPushButton("Add pair"), removePair = new QPushButton("Remove pair");
    addPair->setObjectName("addMirrorPairButton");
    removePair->setObjectName("removeMirrorPairButton");
    pairLayout->addWidget(addPair);
    pairLayout->addWidget(removePair);
    connect(addPair, &QPushButton::clicked, &dialog, [&] {
        pairRow(-1, -1);
        mirrors->scrollToBottom();
    });
    connect(removePair, &QPushButton::clicked, &dialog,
            [&] { delete mirrors->takeTopLevelItem(mirrors->indexOfTopLevelItem(mirrors->currentItem())); });
    form->addRow(pairButtons);
    scroll->setWidget(content);
    layout->addWidget(scroll);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        CharacterProfile next = profile;
        next.configured = true;
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            next.roles.remove(it.key());
            if (it.value()->currentData().toInt() >= 0)
                next.roles[it.key()] = it.value()->currentData().toInt();
        }
        for (auto side : {QString("left"), QString("right")})
            for (auto limb : {QString("Arm"), QString("Leg")}) {
                auto r = side + (limb == "Arm" ? "UpperArm" : "UpperLeg"),
                     m = side + (limb == "Arm" ? "ForeArm" : "Leg"),
                     e = side + (limb == "Arm" ? "Hand" : "Foot");
                bool same = next.roles.value(r, -1) == profile.roles.value(r, -1) &&
                            next.roles.value(m, -1) == profile.roles.value(m, -1) &&
                            next.roles.value(e, -1) == profile.roles.value(e, -1);
                if (!same)
                    next.chains.remove(profile.roles.value(e, -1));
                if (!next.roles.contains(r) || !next.roles.contains(m) || !next.roles.contains(e))
                    continue;
                QVector3D direction;
                for (int axis = 0; axis < 3; ++axis)
                    direction[axis] = float(poles[side + limb][axis]->value());
                int end = next.roles[e];
                auto chain = same && profile.chains.contains(end)
                                 ? profile.chains[end]
                                 : IKChain{next.roles[r], next.roles[m], end, direction};
                chain.pole = direction;
                next.chains[end] = chain;
            }
        next.mirrors.clear();
        for (int row = 0; row < mirrors->topLevelItemCount(); ++row) {
            auto item = mirrors->topLevelItem(row);
            int left = qobject_cast<QComboBox *>(mirrors->itemWidget(item, 0))->currentData().toInt();
            int right = qobject_cast<QComboBox *>(mirrors->itemWidget(item, 1))->currentData().toInt();
            if (left < 0 || right < 0 || left == right) {
                QMessageBox::information(&dialog, "Mirror pair", "Assign two distinct joints to each pair.");
                return;
            }
            next.mirrors[left] = right;
            next.mirrors[right] = left;
        }
        auto check = document().scene;
        check.profile = next;
        QString error;
        if (!check.validateProfile(&error)) {
            QMessageBox::information(&dialog, "Profile assignments", error);
            return;
        }
        document().edit("Character profile", [&](Scene &s) { s.profile = next; }, false);
        dialog.accept();
        report(
            QString("Profile saved: %1 roles, %2 IK chains.").arg(next.roles.size()).arg(next.chains.size()));
    });
    dialog.exec();
}
} // namespace trickster
