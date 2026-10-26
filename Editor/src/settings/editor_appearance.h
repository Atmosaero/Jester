#pragma once
#include "settings/preferences.h"
#include <QColor>
class QApplication;
namespace trickster {
void applyEditorAppearance(QApplication &,const EditorPreferences &);
QColor overlayColor(const char *name,int alpha=255);
}
