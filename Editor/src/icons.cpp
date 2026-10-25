#include "icons.h"
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
namespace trickster {
QIcon icon(Glyph glyph) {
    QIcon result;
    for (bool active : {false, true}) {
        QPixmap pixmap(48, 48);
        pixmap.setDevicePixelRatio(2);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(active ? QColor(70, 192, 239) : QColor(200, 207, 215), 1.65, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        auto line = [&](qreal x, qreal y, qreal a, qreal b) { p.drawLine(QPointF(x, y), QPointF(a, b)); };
        auto poly = [&](std::initializer_list<QPointF> points, bool fill = false) {
            QPolygonF v;
            for (auto pt : points)
                v << pt;
            p.setBrush(fill ? p.pen().color() : QColor(Qt::transparent));
            p.drawPolygon(v);
            p.setBrush(Qt::NoBrush);
        };
        switch (glyph) {
        case Glyph::Note:
            poly({{3,4},{21,4},{21,17},{11,17},{6,21},{6,17},{3,17}});
            line(7,8,17,8);line(7,12,15,12);
            break;
        case Glyph::New:
            p.drawRoundedRect(QRectF(5, 3, 14, 18), 1, 1);
            line(8, 12, 16, 12);
            line(12, 8, 12, 16);
            break;
        case Glyph::Library:
            for (int x : {3, 13})
                for (int y : {3, 13})
                    p.drawRoundedRect(QRectF(x, y, 7, 7), 1, 1);
            break;
        case Glyph::Open:
            poly({{3, 7}, {9, 7}, {11, 9}, {21, 9}, {19, 20}, {3, 20}});
            line(3, 16, 6, 11);
            break;
        case Glyph::Save:
            poly({{4, 3}, {18, 3}, {21, 6}, {21, 21}, {4, 21}});
            p.drawRect(QRectF(8, 3, 8, 6));
            p.drawRect(QRectF(8, 14, 9, 7));
            break;
        case Glyph::Export:
            p.drawRect(QRectF(4, 12, 16, 9));
            line(12, 16, 12, 3);
            line(8, 7, 12, 3);
            line(16, 7, 12, 3);
            break;
        case Glyph::Undo:
        case Glyph::Redo: {
            if (glyph == Glyph::Redo) {
                p.translate(24, 0);
                p.scale(-1, 1);
            }
            QPainterPath a;
            a.moveTo(20, 18);
            a.cubicTo(22, 7, 11, 7, 5, 9);
            p.drawPath(a);
            line(5, 9, 9, 4);
            line(5, 9, 11, 13);
            break;
        }
        case Glyph::Select:
            poly({{5, 3}, {6, 20}, {11, 15}, {15, 22}, {18, 20}, {14, 13}, {21, 12}}, true);
            break;
        case Glyph::Move:
            line(3, 12, 21, 12);
            line(12, 3, 12, 21);
            poly({{3, 12}, {6, 9}, {6, 15}}, true);
            poly({{21, 12}, {18, 9}, {18, 15}}, true);
            poly({{12, 3}, {9, 6}, {15, 6}}, true);
            poly({{12, 21}, {9, 18}, {15, 18}}, true);
            break;
        case Glyph::Rotate:
            p.drawArc(QRectF(4, 4, 16, 16), 30 * 16, 290 * 16);
            poly({{20, 4}, {20, 10}, {15, 7}}, true);
            break;
        case Glyph::Local:
            p.drawEllipse(QRectF(4, 4, 16, 16));
            p.drawEllipse(QRectF(8, 4, 8, 16));
            line(4, 12, 20, 12);
            break;
        case Glyph::Pin:
            poly({{8, 3}, {17, 3}, {15, 9}, {19, 13}, {5, 13}, {9, 9}});
            line(12, 13, 12, 22);
            break;
        case Glyph::Feet:
            p.drawRoundedRect(QRectF(4, 3, 5, 17), 2, 2);
            p.drawRoundedRect(QRectF(14, 3, 5, 17), 2, 2);
            line(4, 16, 9, 16);
            line(14, 16, 19, 16);
            break;
        case Glyph::Mirror:
            line(12, 2, 12, 22);
            poly({{3, 6}, {8, 10}, {8, 18}, {3, 16}});
            poly({{21, 6}, {16, 10}, {16, 18}, {21, 16}});
            break;
        case Glyph::Ghost: {
            QPainterPath path;
            path.moveTo(5, 21);
            path.lineTo(5, 10);
            path.cubicTo(5, 1, 19, 1, 19, 10);
            path.lineTo(19, 21);
            path.lineTo(15, 18);
            path.lineTo(12, 21);
            path.lineTo(9, 18);
            path.closeSubpath();
            p.drawPath(path);
            p.drawEllipse(QRectF(8, 9, 1, 2));
            p.drawEllipse(QRectF(15, 9, 1, 2));
            break;
        }
        case Glyph::Trajectory:
        case Glyph::Ballistic: {
            QPainterPath a;
            a.moveTo(3, 20);
            a.cubicTo(7, -2, 17, -2, 21, 20);
            p.drawPath(a);
            p.setBrush(p.pen().color());
            for (QPointF pt : {QPointF(3, 20), QPointF(12, 4), QPointF(21, 20)})
                p.drawEllipse(pt, 2, 2);
            if (glyph == Glyph::Ballistic) {
                line(12, 9, 12, 18);
                line(9, 15, 12, 18);
                line(15, 15, 12, 18);
            }
            break;
        }
        case Glyph::Mass:
            p.drawEllipse(QRectF(5, 5, 14, 14));
            line(12, 1, 12, 23);
            line(1, 12, 23, 12);
            p.setBrush(p.pen().color());
            p.drawEllipse(QRectF(10, 10, 4, 4));
            break;
        case Glyph::Physics:
            poly({{13, 2}, {5, 13}, {11, 13}, {9, 22}, {20, 9}, {14, 9}}, true);
            break;
        case Glyph::Mesh:
            poly({{4, 7}, {12, 3}, {20, 7}, {20, 17}, {12, 21}, {4, 17}});
            line(4, 7, 12, 12);
            line(20, 7, 12, 12);
            line(12, 12, 12, 21);
            break;
        case Glyph::Bones:
            p.drawEllipse(QRectF(9, 2, 6, 6));
            line(12, 8, 12, 15);
            line(4, 10, 20, 10);
            line(12, 15, 6, 22);
            line(12, 15, 18, 22);
            break;
        case Glyph::Camera:
            p.drawRoundedRect(QRectF(3, 7, 18, 13), 2, 2);
            p.drawEllipse(QRectF(8, 10, 8, 8));
            poly({{7, 7}, {9, 3}, {15, 3}, {17, 7}});
            break;
        case Glyph::Key:
            poly({{12, 3}, {21, 12}, {12, 21}, {3, 12}}, true);
            break;
        case Glyph::Play:
            poly({{7, 4}, {20, 12}, {7, 20}}, true);
            break;
        case Glyph::Pause:
            p.setBrush(p.pen().color());
            p.drawRect(QRectF(6, 4, 4, 16));
            p.drawRect(QRectF(15, 4, 4, 16));
            break;
        case Glyph::Previous:
        case Glyph::Next:
        case Glyph::First:
        case Glyph::Last: {
            if (glyph == Glyph::Next || glyph == Glyph::Last) {
                p.translate(24, 0);
                p.scale(-1, 1);
            }
            line(5, 5, 5, 19);
            poly({{18, 5}, {8, 12}, {18, 19}}, true);
            if (glyph == Glyph::First) {
                line(2, 5, 2, 19);
            }
            break;
        }
        case Glyph::Loop:
            p.drawArc(QRectF(4, 5, 16, 14), 0, 140 * 16);
            p.drawArc(QRectF(4, 5, 16, 14), 180 * 16, 140 * 16);
            poly({{3, 6}, {10, 5}, {6, 12}}, true);
            poly({{21, 18}, {14, 19}, {18, 12}}, true);
            break;
        case Glyph::Settings:
            p.drawEllipse(QRectF(6, 6, 12, 12));
            p.drawEllipse(QRectF(10, 10, 4, 4));
            for (int i = 0; i < 8; ++i) {
                p.save();
                p.translate(12, 12);
                p.rotate(i * 45);
                line(0, 7, 0, 10);
                p.restore();
            }
            break;
        case Glyph::Shapes:
            p.drawEllipse(QRectF(3, 3, 18, 18));
            line(7, 9, 8, 9);
            line(16, 9, 17, 9);
            p.drawArc(QRectF(7, 9, 10, 8), 190 * 16, 160 * 16);
            break;
        case Glyph::Bake:
            poly({{4, 5}, {12, 2}, {20, 5}, {20, 18}, {12, 22}, {4, 18}});
            line(8, 12, 11, 15);
            line(11, 15, 17, 9);
            break;
        case Glyph::Expand:
        case Glyph::Collapse:
            line(6, 12, 18, 12);
            if (glyph == Glyph::Expand)
                line(12, 6, 12, 18);
            p.drawRect(QRectF(3, 3, 18, 18));
            break;
        }
        p.end();
        result.addPixmap(pixmap, QIcon::Normal, active ? QIcon::On : QIcon::Off);
    }
    return result;
}
} // namespace trickster
