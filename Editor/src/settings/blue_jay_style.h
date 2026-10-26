#pragma once
#include <QColor>
class QApplication;
namespace trickster {
class EditorPreferences;
namespace BlueJay {
inline constexpr auto Blue = "#1e70eb";
inline constexpr auto Red = "#d93644";
inline constexpr auto Success = "#58bc61";
inline constexpr auto Warning = "#ffaa22";
inline constexpr auto Error = "#fa2727";
}
void applyBlueJayStyle(QApplication &, const EditorPreferences &);
}
