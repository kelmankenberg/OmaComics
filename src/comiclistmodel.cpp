#include "comiclistmodel.h"

#include <QSet>
#include <utility>

ComicListModel::ComicListModel(QObject *parent) : QAbstractListModel(parent) {}

int ComicListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_comics.size();
}

QVariant ComicListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_comics.size() || role != ComicRole)
        return {};
    return m_comics.at(index.row());
}

QHash<int, QByteArray> ComicListModel::roleNames() const {
    return {{ComicRole, "comic"}};
}

void ComicListModel::synchronize(const QVariantList &comics) {
    QVector<QVariantMap> updated;
    QSet<QString> paths;
    updated.reserve(comics.size());
    for (const QVariant &comic : comics) {
        const QVariantMap book = comic.toMap();
        paths.insert(book.value(QStringLiteral("path")).toString());
        updated.append(book);
    }

    for (int i = m_comics.size() - 1; i >= 0;) {
        if (paths.contains(m_comics.at(i).value(QStringLiteral("path")).toString())) {
            --i;
            continue;
        }
        const int last = i;
        while (i >= 0 && !paths.contains(m_comics.at(i).value(QStringLiteral("path")).toString()))
            --i;
        const int first = i + 1;
        beginRemoveRows({}, first, last);
        m_comics.remove(first, last - first + 1);
        endRemoveRows();
    }
    QSet<QString> existingPaths;
    for (const QVariantMap &book : std::as_const(m_comics))
        existingPaths.insert(book.value(QStringLiteral("path")).toString());
    for (int i = 0; i < updated.size(); ++i) {
        const QString path = updated.at(i).value(QStringLiteral("path")).toString();
        if (i >= m_comics.size() || m_comics.at(i).value(QStringLiteral("path")).toString() != path) {
            int existing = i + 1;
            while (existing < m_comics.size()
                   && m_comics.at(existing).value(QStringLiteral("path")).toString() != path)
                ++existing;
            if (existing < m_comics.size()) {
                beginMoveRows({}, existing, existing, {}, i);
                m_comics.move(existing, i);
                endMoveRows();
            } else {
                int last = i;
                while (last + 1 < updated.size()
                       && !existingPaths.contains(updated.at(last + 1).value(QStringLiteral("path")).toString()))
                    ++last;
                beginInsertRows({}, i, last);
                for (int j = i; j <= last; ++j) {
                    m_comics.insert(j, updated.at(j));
                    existingPaths.insert(updated.at(j).value(QStringLiteral("path")).toString());
                }
                endInsertRows();
                i = last;
                continue;
            }
        }
        if (m_comics.at(i) != updated.at(i)) {
            m_comics[i] = updated.at(i);
            emit dataChanged(index(i), index(i), {ComicRole});
        }
    }
}
