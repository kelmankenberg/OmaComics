#include "comictreemodel.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>

ComicTreeModel::ComicTreeModel(QObject *parent) : QAbstractItemModel(parent) {}

ComicTreeModel::Node *ComicTreeModel::node(const QModelIndex &index) const {
    return index.isValid() ? static_cast<Node *>(index.internalPointer()) : const_cast<Node *>(&m_root);
}

QModelIndex ComicTreeModel::index(int row, int column, const QModelIndex &parent) const {
    const Node *p = node(parent);
    if (column != 0 || row < 0 || row >= int(p->children.size()))
        return {};
    return createIndex(row, column, p->children.at(row).get());
}

QModelIndex ComicTreeModel::parent(const QModelIndex &index) const {
    if (!index.isValid())
        return {};
    Node *p = node(index)->parent;
    return p && p != &m_root ? createIndex(p->row, 0, p) : QModelIndex();
}

int ComicTreeModel::rowCount(const QModelIndex &parent) const {
    return parent.column() > 0 ? 0 : int(node(parent)->children.size());
}

QVariant ComicTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};
    const Node *n = node(index);
    switch (role) {
    case Qt::DisplayRole:
        return n->comic.isEmpty() ? (QFileInfo(n->path).fileName().isEmpty() ? n->path : QFileInfo(n->path).fileName())
                                 : n->comic.value(QStringLiteral("title"));
    case ComicRole: return n->comic;
    case PathRole: return n->path;
    case FolderRole: return n->comic.isEmpty();
    case SelectionRole: return n->selection;
    default: return {};
    }
}

QHash<int, QByteArray> ComicTreeModel::roleNames() const {
    return {{Qt::DisplayRole, "display"}, {ComicRole, "comic"}, {PathRole, "filePath"},
            {FolderRole, "folder"}, {SelectionRole, "selectionState"}};
}

void ComicTreeModel::synchronizeSelection(const QStringList &paths) {
    updateSelection(&m_root, QSet<QString>(paths.cbegin(), paths.cend()));
}

void ComicTreeModel::updateSelection(Node *n, const QSet<QString> &paths) {
    bool selected = false;
    bool unselected = false;
    for (const auto &child : n->children) {
        updateSelection(child.get(), paths);
        selected |= child->selection != Qt::Unchecked;
        unselected |= child->selection != Qt::Checked;
    }
    const Qt::CheckState state = n->comic.isEmpty()
        ? (selected ? (unselected ? Qt::PartiallyChecked : Qt::Checked) : Qt::Unchecked)
        : (paths.contains(n->path) ? Qt::Checked : Qt::Unchecked);
    if (n->selection == state)
        return;
    n->selection = state;
    if (n != &m_root) {
        const QModelIndex i = createIndex(n->row, 0, n);
        emit dataChanged(i, i, {SelectionRole});
    }
}

ComicTreeModel::Node *ComicTreeModel::addFolder(Node *parent, const QString &path) {
    for (const auto &child : parent->children)
        if (child->path == path && child->comic.isEmpty())
            return child.get();
    auto child = std::make_unique<Node>();
    child->path = path;
    child->parent = parent;
    child->row = int(parent->children.size());
    Node *result = child.get();
    parent->children.push_back(std::move(child));
    return result;
}

void ComicTreeModel::synchronize(const QVariantList &comics, const QVariantList &folders) {
    QStringList roots;
    for (const QVariant &folder : folders)
        roots.append(QDir::cleanPath(folder.toString()));
    std::sort(roots.begin(), roots.end(), [](const QString &a, const QString &b) { return a.size() < b.size(); });
    QStringList topRoots;
    for (const QString &root : std::as_const(roots)) {
        if (std::none_of(topRoots.cbegin(), topRoots.cend(), [&](const QString &p) {
                return root == p || root.startsWith(p.endsWith('/') ? p : p + '/');
            }))
            topRoots.append(root);
    }
    QVariantList ordered = comics;
    std::sort(ordered.begin(), ordered.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("path")).toString()
            < b.toMap().value(QStringLiteral("path")).toString();
    });
    QStringList structure = topRoots;
    structure.append(QString());
    for (const QVariant &value : std::as_const(ordered))
        structure.append(value.toMap().value(QStringLiteral("path")).toString());
    if (structure == m_structure) {
        for (const QVariant &value : std::as_const(ordered)) {
            const QVariantMap comic = value.toMap();
            Node *n = m_leaves.value(comic.value(QStringLiteral("path")).toString());
            if (n && n->comic != comic) {
                n->comic = comic;
                const QModelIndex i = createIndex(n->row, 0, n);
                emit dataChanged(i, i);
            }
        }
        return;
    }
    beginResetModel();
    m_structure = structure;
    m_root.children.clear();
    m_leaves.clear();
    for (const QString &root : std::as_const(topRoots))
        addFolder(&m_root, root);
    for (const QVariant &value : std::as_const(ordered)) {
        const QVariantMap comic = value.toMap();
        const QString path = comic.value(QStringLiteral("path")).toString();
        Node *p = &m_root;
        for (const QString &root : std::as_const(topRoots)) {
            if (path.startsWith(root.endsWith('/') ? root : root + '/')) {
                p = addFolder(&m_root, root);
                break;
            }
        }
        const QString directory = QFileInfo(path).absolutePath();
        const QString relative = p == &m_root ? directory : QDir(p->path).relativeFilePath(directory);
        QString current = p == &m_root ? QStringLiteral("/") : p->path;
        for (const QString &part : relative.split('/', Qt::SkipEmptyParts)) {
            if (part == QStringLiteral("."))
                continue;
            current = QDir(current).filePath(part);
            p = addFolder(p, current);
        }
        auto leaf = std::make_unique<Node>();
        leaf->path = path;
        leaf->comic = comic;
        leaf->parent = p;
        leaf->row = int(p->children.size());
        m_leaves.insert(path, leaf.get());
        p->children.push_back(std::move(leaf));
    }
    endResetModel();
}
