#pragma once

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <functional>

class ComicVine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool configured READ configured NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList matches READ matches NOTIFY changed)
    Q_PROPERTY(QVariantMap metadata READ metadata NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)

public:
    explicit ComicVine(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    bool configured() const { return !m_apiKey.isEmpty(); }
    bool busy() const { return m_busy; }
    QVariantList matches() const { return m_matches; }
    QVariantMap metadata() const { return m_metadata; }
    QString message() const { return m_message; }
    Q_INVOKABLE bool setApiKey(const QString &key);
    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void choose(int id);
    Q_INVOKABLE void cancel();

signals:
    void changed();

private:
    void request(const QString &path, const QList<QPair<QString, QString>> &parameters,
                 const std::function<void(const QJsonValue &)> &done);
    void fail(const QString &message);
    void finishMetadata();
    QNetworkAccessManager m_defaultNetwork;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_wait;
    QHash<QString, qint64> m_lastRequest;
    QHash<QString, QJsonValue> m_cache;
    QString m_apiKey;
    QString m_message;
    QVariantList m_matches;
    QVariantMap m_metadata;
    QByteArray m_response;
    bool m_busy = false;
    int m_generation = 0;
};
