#pragma once
#include <QIcon>
namespace trickster {
enum class Glyph {
    New,
    Open,
    Library,
    Save,
    Export,
    Undo,
    Redo,
    Select,
    Move,
    Rotate,
    Local,
    Pin,
    Feet,
    Mirror,
    Ghost,
    Trajectory,
    Mass,
    Physics,
    Ballistic,
    Mesh,
    Bones,
    Camera,
    Key,
    Play,
    Pause,
    Previous,
    Next,
    First,
    Last,
    Loop,
    Settings,
    Shapes,
    Bake,
    Expand,
    Collapse,
    Note
};
QIcon icon(Glyph glyph);
} // namespace trickster
