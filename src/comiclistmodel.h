#pragma once

#include <QAbstractListModel>
#include <QVariantMap>
#include <QVector>

class ComicListModel : public QAbstractListModel {
    Q_OBJECT

public:
    explicit ComicListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void synchronize(const QVariantList &comics);

private:
    enum { ComicRole = Qt::UserRole + 1 };
    QVector<QVariantMap> m_comics;
};
