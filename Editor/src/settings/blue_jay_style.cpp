#include "settings/blue_jay_style.h"
#include "settings/preferences.h"
#include <AzQtComponents/Components/Style.h>
#include <AzQtComponents/Components/StyleManager.h>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <cmath>
#include <algorithm>

namespace trickster {
namespace {
class BlueJayMetrics;
// These are overrides of the SDK's embedded Blue Jay configuration. No SDK
// files are changed, and installed/portable builds use the same resources.
class BlueJayAssets final : public QObject {
  public:
    QTemporaryDir directory;
    QMap<QString, QString> configs;
    QString sheet;
    QPointer<BlueJayMetrics> metrics;
    explicit BlueJayAssets(QApplication &app) : QObject(&app), sheet(app.styleSheet()) {
        for (const auto *name : {"PushButton", "RadioButton", "CheckBox", "ProgressBar", "Slider", "Card",
             "LineEdit", "ComboBox", "BrowseEdit", "BreadCrumbs", "SpinBox", "ScrollBar", "TabWidget",
             "TableView", "Text", "TitleBar", "Menu", "ToolButton", "DockBarButton", "StatusBar",
             "DragAndDrop", "ToolBar", "TreeView", "FilteredSearchWidget", "AssetFolderThumbnailView",
             "ColorPicker", "ColorPicker/PaletteView", "Eyedropper"}) {
            const QString path = QString::fromLatin1(name) + "Config.ini";
            QFile file("AzQtComponentWidgets:" + path);
            if (file.open(QIODevice::ReadOnly)) configs[path] = QString::fromUtf8(file.readAll());
        }
        app.setProperty("jesterBlueJayConfigCount", configs.size());
        if (configs.isEmpty()) qWarning("Blue Jay embedded widget configuration was not found");
    }
};

QString accentColors(QString text, const QColor &accent) {
    // Interactive blues become the Jester accent. Semantic information icons
    // remain the SDK's blue SVG assets; RGB data/axis colors are independent.
    const QMap<QString, QColor> colors{
        {"#1e70eb", accent}, {"#0095f2", accent.lighter(110)}, {"#008de5", accent},
        {"#4b8cef", accent.lighter(125)}, {"#4abaff", accent.lighter(125)},
        {"#94d2ff", accent.lighter(155)}, {"#2a4875", accent.darker(180)}};
    for (auto it = colors.begin(); it != colors.end(); ++it)
        text.replace(it.key(), it.value().name(), Qt::CaseInsensitive);
    return text;
}

QString scaledPixels(const QString &text, double factor) {
    const QRegularExpression expression("([0-9]+(?:\\.[0-9]+)?)px");
    QString result; int end = 0;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += text.mid(end, match.capturedStart() - end);
        result += QString::number(std::max(0, qRound(match.captured(1).toDouble() * factor))) + "px";
        end = match.capturedEnd();
    }
    return result + text.mid(end);
}

QString themeColors(QString text, const QString &theme) {
    if (theme == "dark") return text;
    text.replace(QRegularExpression("(?<=[:\\s])white(?=\\s*[;!])",QRegularExpression::CaseInsensitiveOption),"#ffffff");
    const QMap<QString, QString> light{{"#111111","#b0b0b0"},{"#222222","#ffffff"},{"#333333","#e3e3e3"},
        {"#444444","#f1f1f1"},{"#555555","#bcbcbc"},{"#666666","#d2d2d2"},{"#777777","#c5c5c5"},
        {"#888888","#bbbbbb"},{"#999999","#777777"},{"#cccccc","#333333"},{"#ffffff","#222222"}};
    const QMap<QString, QString> high{{"#111111","#000000"},{"#222222","#000000"},{"#333333","#111111"},
        {"#444444","#000000"},{"#555555","#ffffff"},{"#666666","#111111"},{"#777777","#222222"},
        {"#888888","#cccccc"},{"#999999","#aaaaaa"},{"#cccccc","#ffffff"}};
    const auto &colors = theme == "light" ? light : high;
    const QRegularExpression expression("#[0-9a-fA-F]{6}(?![0-9a-fA-F])");
    QString result; int end = 0; auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next(); result += text.mid(end, match.capturedStart() - end);
        result += colors.value(match.captured().toLower(), match.captured()); end = match.capturedEnd();
    }
    return result + text.mid(end);
}

class BlueJayMetrics final : public QProxyStyle {
    double scale;
    int iconSize;
    bool compact;
  public:
    explicit BlueJayMetrics(const EditorPreferences &preferences) : QProxyStyle(QStyleFactory::create("Fusion")) {update(preferences);}
    void update(const EditorPreferences &preferences) {
        scale=preferences.value("appearance.uiScale").toDouble();
        iconSize=preferences.value("appearance.iconSize").toInt();
        compact=preferences.value("appearance.density")=="compact";
    }
    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override {
        switch (metric) {
        case PM_SmallIconSize: case PM_ToolBarIconSize: case PM_ButtonIconSize: return qRound(iconSize * scale);
        case PM_ToolBarItemMargin: return qRound((compact ? 2 : 5) * scale);
        case PM_ToolBarItemSpacing: return qRound((compact ? 3 : 6) * scale);
        case PM_LayoutHorizontalSpacing: case PM_LayoutVerticalSpacing: return qRound((compact ? 5 : 9) * scale);
        case PM_ButtonMargin: return qRound((compact ? 6 : 10) * scale);
        default: return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }
};
}

void applyBlueJayStyle(QApplication &app, const EditorPreferences &preferences) {
    static QPointer<BlueJayAssets> assets;
    if (!assets) assets = new BlueJayAssets(app);
    const auto appearance = preferences.toJson()["appearance"].toObject();
    const QColor accent(appearance["accent"].toString());
    const double scale = appearance["uiScale"].toDouble();
    const double textScale = appearance["fontSize"].toDouble() / 9.;
    // Preserve the instance tracked by StyleManager's repolish helpers.
    auto *managed = AzQtComponents::StyleManager::styleSheetStyle(nullptr);
    auto *native = qobject_cast<AzQtComponents::Style*>(managed);
    if(!native && managed)native=managed->findChild<AzQtComponents::Style*>();
    if(native && !assets->metrics) {assets->metrics=new BlueJayMetrics(preferences);native->setBaseStyle(assets->metrics);}
    if(assets->metrics)assets->metrics->update(preferences);
    app.setProperty("jesterNativeBlueJay",bool(native));
    auto sheet = scaledPixels(themeColors(accentColors(assets->sheet, accent), appearance["theme"].toString()), scale * textScale);
    // Blue Jay checkbox/radio/toggle states use SVGs rather than palette brushes.
    // Recolor only interactive assets; status/information icons stay semantic.
    const QRegularExpression iconPaths("(:/[^\\s\\\"\\)]+\\.svg)",QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression interactive("/(?:checkbox|radiobutton|radio-button|radio|toggleswitch|SpinBox)/",QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression hex("#[0-9a-fA-F]{6}(?![0-9a-fA-F])");
    auto paths=iconPaths.globalMatch(sheet);QMap<QString,QString> rewritten;
    while(paths.hasNext()) {
        const auto path=paths.next().captured(1);if(rewritten.contains(path) || !path.contains(interactive))continue;
        QFile source(path);if(!source.open(QIODevice::ReadOnly))continue;
        auto svg=QString::fromUtf8(source.readAll());auto colors=hex.globalMatch(svg);QMap<QString,QString> replacements;bool hasAccent=false;
        while(colors.hasNext()) {const auto code=colors.next().captured();QColor color(code);
            if(color.hsvHue()>=195 && color.hsvHue()<=250 && color.hsvSaturationF()>.2) {
                hasAccent=true;
                if(accent!=QColor(BlueJay::Blue))
                    replacements[code]=QColor::fromHsvF(accent.hsvHueF(),accent.hsvSaturationF()*color.hsvSaturationF(),std::clamp(accent.valueF()*color.valueF()/.92,0.,1.)).name();
            }}
        for(auto it=replacements.begin();it!=replacements.end();++it)svg.replace(it.key(),it.value(),Qt::CaseInsensitive);
        if(!hasAccent || path.contains("/SpinBox/"))svg=themeColors(svg,appearance["theme"].toString());
        const auto name=assets->directory.path()+"/"+QString::number(qHash(path+accent.name()+appearance["theme"].toString()),16)+".svg";
        QSaveFile file(name);if(file.open(QIODevice::WriteOnly)) {file.write(svg.toUtf8());if(file.commit())rewritten[path]=QDir::fromNativeSeparators(name);}
    }
    for(auto it=rewritten.begin();it!=rewritten.end();++it) {
        // Quote local paths so Windows user-profile names may contain spaces.
        const QRegularExpression url("url\\(\\s*[\"']?"+QRegularExpression::escape(it.key())+"[\"']?\\s*\\)");
        sheet.replace(url,"url(\""+it.value()+"\")");
    }
    app.setProperty("jesterBlueJayRecoloredIcons",rewritten.size());
    QString family = appearance["fontFamily"].toString(); family.replace('\\', "\\\\"); family.replace('"', "\\\"");
    sheet.replace(QRegularExpression("font-family\\s*:[^;{}]+;"), "font-family: \"" + family + "\";");
    QFile overrides(":/jester/bluejay/overrides.qss");
    if (overrides.open(QIODevice::ReadOnly)) {
        auto extra = QString::fromUtf8(overrides.readAll());
        extra.replace("@accent@", accent.name());
        extra.replace("@accentHover@", accent.lighter(115).name());
        extra.replace("@blue@", BlueJay::Blue);
        sheet += scaledPixels(themeColors(extra, appearance["theme"].toString()), scale * textScale);
    }
    // Only change the sheet and lowest-level metrics. StyleManager continues
    // to own the original AzQtComponents::Style throughout the application.
    app.setStyleSheet(sheet);
    app.setProperty("jesterDesignSystem", "O3DE Blue Jay / Jester");
}
}
