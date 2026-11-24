#include "comicvine.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextDocument>
#include <QUrlQuery>
#include <cmath>

namespace {
constexpr qsizetype maxResponseBytes = 2 * 1024 * 1024;
constexpr int requestInterval = 20000;

int identifier(const QJsonValue &value) {
    const double n = value.toDouble();
    return std::isfinite(n) && n > 0 && n <= 2147483647 && n == std::floor(n) ? int(n) : 0;
}

QString plain(const QJsonValue &value, int limit = 256) {
    class TextOnlyDocument : public QTextDocument {
        QVariant loadResource(int, const QUrl &) override { return {}; }
    };
    const QString text = value.toString().left(16000);
    TextOnlyDocument document;
    document.setHtml(text);
    QString result = document.toPlainText();
    result.remove(QChar::ObjectReplacementCharacter);
    return result.trimmed().left(limit);
}
}

ComicVine::ComicVine(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_network(network ? network : &m_defaultNetwork) {
    m_wait.setSingleShot(true);
}

bool ComicVine::setApiKey(const QString &key) {
    cancel();
    const QString cleaned = key.trimmed();
    if (!cleaned.isEmpty() && !QRegularExpression(QStringLiteral("^[A-Za-z0-9]{16,128}$")).match(cleaned).hasMatch()) {
        fail(tr("Enter a valid Comic Vine API key."));
        return false;
    }
    m_apiKey = cleaned;
    m_cache.clear();
    m_message.clear();
    emit changed();
    return true;
}

void ComicVine::cancel() {
    ++m_generation;
    m_wait.stop();
    m_wait.disconnect(this);
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
    m_busy = false;
    m_matches.clear();
    m_metadata.clear();
    m_message.clear();
    emit changed();
}

void ComicVine::fail(const QString &message) {
    m_busy = false;
    m_metadata.clear();
    m_message = message;
    emit changed();
}

void ComicVine::request(const QString &path, const QList<QPair<QString, QString>> &parameters,
                        const std::function<void(const QJsonValue &)> &done) {
    QUrl url(QStringLiteral("https://comicvine.gamespot.com/api/") + path);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    for (const auto &parameter : parameters)
        query.addQueryItem(parameter.first, parameter.second);
    const QString cacheKey = path + query.toString(QUrl::FullyEncoded);
    if (m_cache.contains(cacheKey)) {
        done(m_cache.value(cacheKey));
        return;
    }
    const QString resource = path.section('/', 0, 0);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 delay = m_lastRequest.value(resource) + requestInterval - now;
    if (delay > 0) {
        m_message = tr("Waiting for Comic Vine's request limit…");
        m_wait.disconnect(this);
        connect(&m_wait, &QTimer::timeout, this, [this, path, parameters, done] {
            request(path, parameters, done);
        });
        m_wait.start(int(qMin<qint64>(delay, requestInterval)));
        emit changed();
        return;
    }
    m_lastRequest.insert(resource, now);
    query.addQueryItem(QStringLiteral("api_key"), m_apiKey);
    url.setQuery(query);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "OmaComics/0.1 (personal comic library)");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(15000);
    m_response.clear();
    m_message = tr("Contacting Comic Vine…");
    QNetworkReply *reply = m_network->get(req);
    m_reply = reply;
    reply->setReadBufferSize(maxResponseBytes + 1);
    const int generation = m_generation;
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(20000);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        const QByteArray chunk = reply->readAll();
        if (m_response.size() + chunk.size() > maxResponseBytes) {
            reply->setProperty("oversized", true);
            reply->abort();
        } else {
            m_response.append(chunk);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, cacheKey, done] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        m_reply.clear();
        if (reply->property("oversized").toBool()) {
            fail(tr("Comic Vine returned too much data."));
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 429 || status == 420) {
            m_lastRequest.insert(reply->url().path().section('/', 2, 2),
                                 QDateTime::currentMSecsSinceEpoch() + 60000);
            fail(tr("Comic Vine is rate limiting requests. Wait a minute before trying again."));
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            fail(tr("Comic Vine request failed. Check your connection and API key, then try again."));
            return;
        }
        const QByteArray tail = reply->readAll();
        if (m_response.size() + tail.size() > maxResponseBytes) {
            fail(tr("Comic Vine returned too much data."));
            return;
        }
        m_response.append(tail);
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(m_response, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fail(tr("Comic Vine returned an invalid response."));
            return;
        }
        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("status_code")).toInt() != 1) {
            fail(root.value(QStringLiteral("status_code")).toInt() == 100
                     ? tr("Comic Vine rejected the API key.")
                     : tr("Comic Vine could not complete this lookup."));
            return;
        }
        const QJsonValue results = root.value(QStringLiteral("results"));
        if (m_cache.size() >= 100)
            m_cache.clear();
        m_cache.insert(cacheKey, results);
        done(results);
    });
    emit changed();
}

void ComicVine::search(const QString &query) {
    cancel();
    if (!configured()) {
        fail(tr("Set your Comic Vine API key before searching."));
        return;
    }
    const QString cleaned = query.trimmed();
    if (cleaned.isEmpty() || cleaned.size() > 200) {
        fail(tr("Enter a search of 1–200 characters."));
        return;
    }
    m_busy = true;
    request(QStringLiteral("search/"),
            {{QStringLiteral("query"), cleaned}, {QStringLiteral("resources"), QStringLiteral("issue")},
             {QStringLiteral("limit"), QStringLiteral("10")},
             {QStringLiteral("field_list"), QStringLiteral("id,name,issue_number,volume,cover_date")}},
            [this](const QJsonValue &results) {
        if (!results.isArray() || results.toArray().size() > 10) {
            fail(tr("Comic Vine returned an invalid response."));
            return;
        }
        for (const QJsonValue &value : results.toArray()) {
            const QJsonObject issue = value.toObject();
            const int id = identifier(issue.value(QStringLiteral("id")));
            if (!id) {
                m_matches.clear();
                fail(tr("Comic Vine returned an invalid response."));
                return;
            }
            m_matches.append(QVariantMap{
                {QStringLiteral("id"), id},
                {QStringLiteral("title"), plain(issue.value(QStringLiteral("name")))},
                {QStringLiteral("series"), plain(issue.value(QStringLiteral("volume")).toObject().value(QStringLiteral("name")))},
                {QStringLiteral("number"), plain(issue.value(QStringLiteral("issue_number")))},
                {QStringLiteral("date"), plain(issue.value(QStringLiteral("cover_date")), 10)}});
        }
        m_busy = false;
        m_message = m_matches.isEmpty() ? tr("No matches. Edit the search and try again.") : QString();
        emit changed();
    });
}

void ComicVine::choose(int id) {
    if (m_busy)
        return;
    bool found = false;
    for (const QVariant &match : std::as_const(m_matches))
        found = found || match.toMap().value(QStringLiteral("id")).toInt() == id;
    if (!found) {
        fail(tr("Select a Comic Vine search result first."));
        return;
    }
    m_busy = true;
    m_metadata.clear();
    request(QStringLiteral("issue/4000-%1/").arg(id),
            {{QStringLiteral("field_list"),
              QStringLiteral("id,name,issue_number,volume,cover_date,description,deck,person_credits")}},
            [this, id](const QJsonValue &results) {
        const QJsonObject issue = results.toObject();
        if (identifier(issue.value(QStringLiteral("id"))) != id) {
            fail(tr("Comic Vine returned an invalid response."));
            return;
        }
        m_metadata = {
            {QStringLiteral("title"), plain(issue.value(QStringLiteral("name")))},
            {QStringLiteral("series"), plain(issue.value(QStringLiteral("volume")).toObject().value(QStringLiteral("name")))},
            {QStringLiteral("number"), plain(issue.value(QStringLiteral("issue_number")))},
            {QStringLiteral("summary"), plain(issue.value(QStringLiteral("description")), 4000)},
            {QStringLiteral("comicVineId"), id},
            {QStringLiteral("sourceUrl"), QStringLiteral("https://comicvine.gamespot.com/issue/4000-%1/").arg(id)}};
        if (m_metadata.value(QStringLiteral("summary")).toString().isEmpty())
            m_metadata.insert(QStringLiteral("summary"), plain(issue.value(QStringLiteral("deck")), 4000));
        const QString date = issue.value(QStringLiteral("cover_date")).toString();
        if (QRegularExpression(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$")).match(date).hasMatch())
            m_metadata.insert(QStringLiteral("year"), date.left(4));
        QStringList writers;
        const QJsonArray credits = issue.value(QStringLiteral("person_credits")).toArray();
        for (int i = 0; i < qMin(credits.size(), 100); ++i) {
            const QJsonObject credit = credits.at(i).toObject();
            if (credit.value(QStringLiteral("role")).toString().contains(QStringLiteral("writer"), Qt::CaseInsensitive)) {
                const QString writer = plain(credit.value(QStringLiteral("name")));
                if (!writer.isEmpty() && !writers.contains(writer))
                    writers.append(writer);
            }
        }
        m_metadata.insert(QStringLiteral("writer"), writers.join(QStringLiteral(", ")).left(256));
        const int volumeId = identifier(issue.value(QStringLiteral("volume")).toObject().value(QStringLiteral("id")));
        if (!volumeId) {
            finishMetadata();
            return;
        }
        request(QStringLiteral("volume/4050-%1/").arg(volumeId),
                {{QStringLiteral("field_list"), QStringLiteral("id,publisher")}},
                [this, volumeId](const QJsonValue &volume) {
            if (identifier(volume.toObject().value(QStringLiteral("id"))) != volumeId) {
                fail(tr("Comic Vine returned an invalid response."));
                return;
            }
            m_metadata.insert(QStringLiteral("publisher"), plain(volume.toObject().value(QStringLiteral("publisher"))
                                                                     .toObject().value(QStringLiteral("name"))));
            finishMetadata();
        });
    });
}

void ComicVine::finishMetadata() {
    m_busy = false;
    m_message.clear();
    emit changed();
}
