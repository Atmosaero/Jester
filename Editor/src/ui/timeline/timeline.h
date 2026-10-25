#pragma once
#include "core/scene/document.h"
#include <QScrollBar>
#include <QSet>
#include <QWidget>
namespace trickster {
class Timeline : public QWidget {
    Q_OBJECT
  public:
    explicit Timeline(Document *d, QWidget *parent = nullptr);
    void fit();
    void editContext();
    void insertKey(bool neutral = false);
    QString insertionScope() const;
    bool selectTrack(int node, int channels = 7, bool source = false);
    double lastPaintMs = 0;
  signals:
    void scopeChanged(QString scope);
    void graphRequested();
    void notesRequested();

  public:
    int selectedKeyCount() const {
        return int(marks.size());
    }
    QList<int> navigationKeys() const;
    void setDocument(Document *document);
    void setOnlySelected(bool value);
    int selectedKey = -1;
    float framePosition(int frame) const {
        return x(frame);
    }
    int summaryTrackCenter(bool layer) const {
        return rowY(layer ? 1 : 0);
    }
    int trackCenter(int node, int channel = 7, bool source = false) const;
    int contextTrackCenter(const QString &id) const;
    static constexpr int trackWidth = 190;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

  private:
    enum Kind {
        Source,
        Layer,
        SourceBone,
        LayerBone,
        Controller,
        SourceController,
        ContactTrack,
        ContextTrack,
        CurveTrack,
        DiagnosticTrack
    };
    struct Row {
        Kind kind = Source;
        int node = -1, channels = 7, depth = 0;
        QString label;
        std::vector<int> nodes;
        QString reference;
    };
    struct Mark {
        Row scope;
        int frame;
    };
    std::vector<Mark> marks, copiedMarks;
    QString contextDrag;
    QJsonObject contextOriginal;
    int contextEdge = 0;
    double contextPress = 0, contextStart = 0, contextEnd = 0;
    bool selectingTime = false;
    double selectionAnchor = 0;
    QMap<int, CorrectionKey> copiedLayers;
    QMap<int, Key> copiedPoses;
    bool selectingKeys = false, selectingTrack = false;
    QRectF keyRect;
    QSet<int> filterNodes;
    quint64 selectionRevision = 0;
    QJsonArray keyRequests() const;
    bool marked(const Row &, int frame) const;
    bool transformSelection(int delta, bool copy, bool remove = false);
    Document *doc;
    QScrollBar *scroll;
    float start = 0, scale = 8;
    bool sourceDetails = false;
    bool clipboardWorld = false;
    bool fitted = true, dragKey = false, scrubbing = false, copyDrag = false, onlySelected = true;
    int dragFrom = -1, dragTo = -1, selectedRow = 0;
    quint64 dragRevision = 0;
    QString dragLayer, selectionLayer;
    Row dragScope;
    QSet<QString> expanded;
    mutable std::vector<Row> rowCache;
    mutable quint64 rowCacheAnalysis = ~quint64(0);
    mutable quint64 rowCacheMotion = ~quint64(0), rowCacheMetadata = ~quint64(0);
    mutable QSet<int> rowCacheFilter;
    mutable QSet<QString> rowCacheExpanded;
    mutable bool rowCacheSelected = false, rowCacheSource = false;
    std::vector<Row> rows() const;
    float x(float frame) const;
    int frameAt(float position) const;
    int rowAt(float y) const;
    int rowY(int index) const;
    bool editable(const Row &) const;
    mutable QMap<QString, QList<float>> timeCache;
    mutable quint64 timeCacheRevision = ~quint64(0);
    QList<float> times(const Row &) const;
    int keyAt(const Row &, float position) const;
    QString scopeName(const Row &) const;
    QString expansionId(const Row &) const;
    void syncSelection();
    void copy();
    void paste();
};
} // namespace trickster
