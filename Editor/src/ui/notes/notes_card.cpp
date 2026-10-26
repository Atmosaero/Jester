#include "ui/widgets/localized_ui.h"
#include "settings/preferences_service.h"
#include "core/scene/runtime_error.h"
#include "ui/notes/notes_card.h"
#include "ui/notes/notes_ui.h"
#include "ui/window/window.h"
#include "core/scene/edit_service.h"
#include "core/annotations/motion_annotations.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>
#include <QToolButton>
#include <QStyle>
#include <QVBoxLayout>
namespace trickster {
NotesCard::NotesCard(Window *w, NotesController *c, Document *d, QJsonObject value, bool isNew)
    : QFrame(w->viewport->overlayWidget()), document(d), original(value), structure(d->structureRevision), fresh(isNew), window(w), controller(c) {
    setObjectName("noteCard");
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    auto appearance=[this] {setStyleSheet("#noteCard { border: 1px solid "+PreferencesService::instance().current().value("overlays.notes").toString()+"; border-radius: 6px; background: palette(window); }");};
    connect(&PreferencesService::instance(),&PreferencesService::changed,this,appearance);appearance();
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(5);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true); scroll->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored); scroll->setMinimumSize(0,0); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll, 1);
    auto body = new QWidget;
    scroll->setWidget(body);
    auto content = new QVBoxLayout(body); content->setContentsMargins(0,0,4,0); content->setSpacing(5);
    address = new QLabel; address->setObjectName("noteAddress"); address->setWordWrap(true);
    content->addWidget(address);
    text = new QTextEdit;
    text->setObjectName("noteText"); text->setAcceptRichText(false);
    bindUiText(text,"placeholderText","Write a note...  Ctrl+Enter to save");
    text->setMinimumHeight(64); text->setMaximumHeight(112);
    text->setPlainText(original["description"].toString(original["name"].toString()));
    content->addWidget(text);
    auto more = new QToolButton; bindUiText(more,"text","Advanced"); more->setCheckable(true);
    more->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); more->setArrowType(Qt::RightArrow);
    content->addWidget(more);
    advanced = new QWidget; advanced->hide();
    auto form = new QFormLayout(advanced); form->setContentsMargins(0,0,0,0);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    start = new QDoubleSpinBox; end = new QDoubleSpinBox;
    start->setObjectName("noteStartSeconds"); end->setObjectName("noteEndSeconds");
    for(auto box:{start,end}) {box->setRange(-1e7,1e7); box->setDecimals(12); box->setKeyboardTracking(false);}
    start->setValue(original["start"].toDouble()); end->setValue(original["end"].toDouble());
    initialStart=start->value();initialEnd=end->value();
    connect(start,&QDoubleSpinBox::valueChanged,this,[this]{refreshAddress();});
    connect(end,&QDoubleSpinBox::valueChanged,this,[this]{refreshAddress();});
    addTranslatedRow(form,"Start (seconds)",start); addTranslatedRow(form,"End (seconds)",end);
    origin=new QComboBox;origin->setObjectName("noteOrigin"); addTranslatedItems(origin,{"intention","observation","hypothesis"}); setChoiceText(origin,original["provenance"].toString("intention"));
    type=new QComboBox; type->setObjectName("noteType"); type->setEditable(true); addTranslatedItems(type,{"event","phase","contact","allowed-contact","pose","diagnostic"}); setChoiceText(type,original["type"].toString("event"));
    addTranslatedRow(form,"Origin",origin); addTranslatedRow(form,"Type",type);
    bindings=new QListWidget;bindings->setObjectName("noteBindings");bindings->setMaximumHeight(96);
    for(int n=0;n<int(d->scene.nodes.size());++n){auto item=new QListWidgetItem(d->scene.nodes[n].name,bindings);item->setData(Qt::UserRole,n);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(original["nodes"].toArray().contains(n)?Qt::Checked:Qt::Unchecked);}
    for(auto n:original["nodes"].toArray())if(n.toInt()>=int(d->scene.nodes.size())){auto item=new QListWidgetItem(uiText("Missing bone #%1").arg(n.toInt()),bindings);item->setData(Qt::UserRole,n.toInt());item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(Qt::Checked);}
    addTranslatedRow(form,"Related bones (none = character)",bindings);
    connect(bindings,&QListWidget::itemChanged,this,[this]{bindingsEdited=true;refreshAddress();});
    tags=new QLineEdit; QStringList tagNames; for(auto v:original["tags"].toArray())tagNames<<v.toString(); tags->setText(tagNames.join(", "));
    addTranslatedRow(form,"Tags (comma separated)",tags);
    expectations=new QPlainTextEdit; expectations->setMaximumHeight(90);
    expectations->setPlainText(QString::fromUtf8(QJsonDocument(original["expectations"].toArray()).toJson(QJsonDocument::Compact)));
    expectations->document()->setModified(false);tags->setModified(false);
    addTranslatedRow(form,"Authored expectations (JSON)",expectations);
    auto disclaimer=translatedWidget<QLabel>("Authored context only. Text does not create constraints or certify a contact."); disclaimer->setWordWrap(true); form->addRow(disclaimer);
    if(original.contains("attachment")) {
        auto attachment=original["attachment"].toObject();
        auto status=new QLabel;
        bindUiTextFactory(status,"text",[attachment] {return uiText("Key attachment: %1 / frame %2").arg(ownedValueLabel(attachment["status"].toString())).arg(attachment["frame"].toInt());});
        status->setWordWrap(true);form->addRow(status);
    }
    attach=translatedWidget<QPushButton>("Attach to key at captured frame");
    attachFrame=annotationFrame(original["start"].toDouble(),d->scene.fps);
    attachLayer=d->scene.correction().present?d->scene.correction().id:QString();
    form->addRow(attach);
    auto detach=translatedWidget<QPushButton>("Detach from key"); form->addRow(detach);
    connect(attach,&QPushButton::clicked,this,[this]{attachRequested=true;detachRequested=false;bindUiText(attach,"text","Will attach when saved");});
    connect(detach,&QPushButton::clicked,this,[this]{detachRequested=true;attachRequested=false;bindUiText(attach,"text","Will detach when saved");});
    content->addWidget(advanced);
    content->addStretch();
    connect(more,&QToolButton::toggled,this,[this,more](bool on){advanced->setVisible(on);setProperty("notesExpanded",on);more->setArrowType(on?Qt::DownArrow:Qt::RightArrow);controller->placeCard(this);});
    error=new QLabel; error->setObjectName("noteError"); error->setWordWrap(true); error->setStyleSheet("color:palette(text);font-weight:600;");error->hide();
    layout->addWidget(error);
    auto buttons=new QHBoxLayout;
    auto saveButton=translatedWidget<QPushButton>("Save"); saveButton->setObjectName("noteSave");bindUiText(saveButton,"toolTip","Save note (Ctrl+Enter)");
    auto cancel=translatedWidget<QPushButton>("Cancel"); cancel->setObjectName("noteCancel");
    auto removeButton=new QToolButton;removeButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));bindUiText(removeButton,"toolTip","Delete note (Undo available)");bindUiText(removeButton,"accessibleName","Delete note");removeButton->setObjectName("noteDelete");removeButton->setVisible(!fresh);
    buttons->addWidget(removeButton); buttons->addStretch();buttons->addWidget(cancel);buttons->addWidget(saveButton);layout->addLayout(buttons);
    connect(saveButton,&QPushButton::clicked,this,[this]{save();});
    connect(cancel,&QPushButton::clicked,this,[this]{hide();deleteLater();window->activateWindow();window->viewport->setFocus();});
    connect(removeButton,&QToolButton::clicked,this,[this]{remove();});
    connect(d,&QObject::destroyed,this,&QObject::deleteLater);
    // Prevent window animation shortcuts in every editable field, including advanced controls.
    for(auto widget:findChildren<QWidget*>())widget->installEventFilter(this);
    installEventFilter(this);
    refreshAddress();
    connect(&LocalizationService::instance(),&LocalizationService::languageChanged,this,[this]{refreshAddress();});
}
bool NotesCard::dirty() const {
    return fresh || text->toPlainText()!=original["description"].toString(original["name"].toString()) ||
           start->value()!=initialStart || end->value()!=initialEnd ||
           choiceText(origin)!=original["provenance"].toString() || choiceText(type)!=original["type"].toString() || attachRequested || detachRequested ||
           bindingsEdited || tags->isModified() || expectations->document()->isModified();
}
QJsonArray NotesCard::editedBindings() const {
    QSet<int> selected;
    for(int i=0;i<bindings->count();++i)if(bindings->item(i)->checkState()==Qt::Checked)selected.insert(bindings->item(i)->data(Qt::UserRole).toInt());
    QJsonArray result;
    for(auto n:original["nodes"].toArray())if(selected.remove(n.toInt()))result.append(n);
    auto remaining=selected.values();std::sort(remaining.begin(),remaining.end());for(int n:remaining)result.append(n);
    return result;
}
void NotesCard::focusText(){show();raise();QWidget::window()->raise();QWidget::window()->activateWindow();text->setFocus(Qt::OtherFocusReason);}
void NotesCard::refreshAddress(){
    if(!document)return;
    auto display=original;
    if(bindingsEdited)display["nodes"]=editedBindings();
    if(start->value()!=initialStart)display["start"]=start->value();
    if(end->value()!=initialEnd)display["end"]=end->value();
    auto summary=annotationSummary(document->scene,display);
    QString status;
    if(summary["outsideClip"].toBool())status+=" | Outside clip";
    if(summary["missingBindings"].toBool())status+=" | Missing binding";
    address->setText(noteBindingText(document->scene,display)+" | "+noteTimeLabel(display,document->scene.fps)+status);
    address->setToolTip(uiText("%1 | %2 seconds; address captured when opened").arg(start->value(),0,'g',15).arg(end->value(),0,'g',15));
}
bool NotesCard::eventFilter(QObject *object,QEvent *event){
    if(event->type()==QEvent::ShortcutOverride){
        auto key=static_cast<QKeyEvent*>(event);
        // Text and animation shortcuts belong to the editor field while typing.
        const bool global=key->modifiers().testFlag(Qt::ControlModifier) && (key->key()==Qt::Key_S || key->key()==Qt::Key_O || key->key()==Qt::Key_Comma);
        if(!global && key->key()!=Qt::Key_Control && key->key()!=Qt::Key_Shift && key->key()!=Qt::Key_Alt){event->accept();return true;}
    }
    if(event->type()==QEvent::KeyPress){auto key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Escape){hide();deleteLater();window->activateWindow();window->viewport->setFocus();return true;}
        if((key->key()==Qt::Key_Return||key->key()==Qt::Key_Enter)&&(key->modifiers()&Qt::ControlModifier)){save();return true;}
    }
    return QFrame::eventFilter(object,event);
}
void NotesCard::save(){
    auto fail=[&](QString message){bindUiTextFactory(error,"text",[message] {return translatedDiagnostic(message);});error->show();};
    if(!document || &window->document()!=document || document->structureRevision!=structure){fail(QString("The original scene is no longer available. Cancel this draft."));return;}
    if(document->inTransaction() || window->viewport->isDragging()){fail(QString("Finish the current pose edit first."));return;}
    QString fullText=text->toPlainText();
    if(fullText.trimmed().isEmpty()){fail(QString("Write a note before saving."));text->setFocus();return;}
    if(!fresh){bool found=false;for(auto v:document->scene.annotations)if(v.toObject()["id"]==original["id"]){found=true;if(v.toObject()!=original){fail(QString("This note changed outside the card. Cancel and reopen it to keep those changes."));return;}}
        if(!found){fail(QString("This note was deleted. Cancel this draft."));return;}}
    if(!fresh && !dirty()){hide();deleteLater();window->activateWindow();window->viewport->setFocus();return;}
    QJsonObject patch{{"description",fullText},{"name",fullText.section('\n',0,0).trimmed().left(72)}};
    if(patch["name"].toString().isEmpty())patch["name"]=fullText.trimmed().section('\n',0,0).left(72);
    // Patch only fields that changed: preserve unknown fields, expectations and attachments byte-for-byte.
    if(fresh || start->value()!=initialStart)patch["start"]=start->value();
    if(fresh || end->value()!=initialEnd)patch["end"]=end->value();
    if(fresh){patch["nodes"]=original["nodes"];patch["provenance"]=choiceText(origin);patch["type"]=choiceText(type);}
    if(choiceText(origin)!=original["provenance"].toString())patch["provenance"]=choiceText(origin);
    if(choiceText(type)!=original["type"].toString())patch["type"]=choiceText(type);
    if(bindingsEdited)patch["nodes"]=editedBindings();
    if(tags->isModified()){QJsonArray values;for(auto tag:tags->text().split(',',Qt::SkipEmptyParts))values.append(tag.trimmed());patch["tags"]=values;}
    if(expectations->document()->isModified()){
        QJsonParseError parse;auto json=QJsonDocument::fromJson(expectations->toPlainText().toUtf8(),&parse);
        if(parse.error!=QJsonParseError::NoError || !json.isArray()){fail(QString("Expectations must be a JSON array."));return;}patch["expectations"]=json.array();
    }
    try {
        if(!attachRequested && !detachRequested){
            auto result=EditService::execute(*document,"motion_annotations",{{"operation",fresh?"add":"set"},{"id",original["id"]},{"annotation",patch}},fresh?"Add note":"Edit note");
            document->activeAnnotation=result["annotation"].toObject()["id"].toString();
            hide();deleteLater();window->viewport->setFocus();window->report(QString("Note saved"));controller->refresh();return;
        }
        Scene staged=document->scene;
        auto result=editAnnotations(staged,{{"operation",fresh?"add":"set"},{"id",original["id"]},{"annotation",patch}});
        auto id=result["annotation"].toObject()["id"].toString();
        if(attachRequested)editAnnotations(staged,{{"operation","attach"},{"id",id},{"frame",attachFrame},{"layer",attachLayer}});
        else if(detachRequested)editAnnotations(staged,{{"operation","detach"},{"id",id}});
        QString reason;if(!staged.validate(&reason))throw trickster::RuntimeError(reason.toStdString());
        // Text edits never replace pose, keys or render data. The explicit key-ID attachment can update IDs.
        auto notes=staged.annotations;auto keys=staged.keys;auto layers=staged.layers;
        document->edit(fresh?"Add note":"Edit note",[&](Scene &s){s.annotations=notes;if(attachRequested){s.keys=keys;s.layers=layers;}},false,true);
        document->activeAnnotation=id;
        hide();deleteLater();window->activateWindow();window->viewport->setFocus();
        window->report(QString("Note saved"));controller->refresh();
    }catch(const std::exception &e){fail(QString::fromUtf8(e.what()));}
}
void NotesCard::remove(){
    if(!document || &window->document()!=document || document->structureRevision!=structure)return;
    try {EditService::execute(*document,"motion_annotations",{{"operation","remove"},{"id",original["id"]}},"Delete note");hide();deleteLater();window->activateWindow();window->viewport->setFocus();}
    catch(const std::exception &e){error->setText(e.what());error->show();}
}
} // namespace trickster
