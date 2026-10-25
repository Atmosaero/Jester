#pragma once
#include <QAbstractTableModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#include <functional>
#include <vector>
namespace trickster {
class AnalysisTableModel : public QAbstractTableModel {
  public:
    using QAbstractTableModel::QAbstractTableModel;
    void setReport(QJsonObject);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role = Qt::DisplayRole) const override;
    double seconds(int row) const;
    int node(int row) const;

  private:
    QJsonObject report;
    QJsonArray rows;
    QVector<QPair<int, int>> indices;
    QStringList columns;
    int mode = 0;
};
class ComputedMotionPlot : public QWidget {
  public:
    explicit ComputedMotionPlot(QWidget *parent = nullptr);
    void setReport(QJsonObject);
    void setMetric(QString name, int component, int node);
    std::function<void(double)> seek;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

  private:
    struct Sample {
        double t, value;
        bool gap;
    };
    QJsonObject report;
    QString metric = "speed";
    int component = 0, node = 0;
    std::vector<Sample> samples;
    double first = 0, last = 1, low = 0, high = 1;
    void rebuild();
};
} // namespace trickster
