#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QSet>
#include <QVariantMap>
#include <memory>
#include <vector>

class ComicTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit ComicTreeModel(QObject *parent = nullptr);
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return 1; }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void synchronize(const QVariantList &comics, const QVariantList &folders);
    void synchronizeSelection(const QStringList &paths);

private:
    enum { ComicRole = Qt::UserRole + 1, PathRole, FolderRole, SelectionRole };
    struct Node {
        QString path;
        QVariantMap comic;
        Node *parent = nullptr;
        int row = 0;
        Qt::CheckState selection = Qt::Unchecked;
        std::vector<std::unique_ptr<Node>> children;
    };
    Node *node(const QModelIndex &index) const;
    Node *addFolder(Node *parent, const QString &path);
    void updateSelection(Node *node, const QSet<QString> &paths);
    Node m_root;
    QStringList m_structure;
    QHash<QString, Node *> m_leaves;
};
