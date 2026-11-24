#include <QDir>
#include <QDirIterator>
#include <QDesktopServices>
#include <QAbstractItemModelTester>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJSEngine>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QProcess>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QtTest>

#include "backend.h"
#include "folderpicker.h"
#include "theme.h"
#include "translator.h"

namespace {

class FeedbackBrowser : public QObject {
    Q_OBJECT
public:
    FeedbackBrowser() { QDesktopServices::setUrlHandler("https", this, "open"); }
    ~FeedbackBrowser() override { QDesktopServices::unsetUrlHandler("https"); }
    QList<QUrl> urls;
public slots:
    void open(const QUrl &url) { urls.append(url); }
};

class FakeReply : public QNetworkReply {
public:
    FakeReply(const QNetworkRequest &request, const QByteArray &payload, int status, bool held, QObject *parent)
        : QNetworkReply(parent), m_payload(payload) {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        open(QIODevice::ReadOnly);
        if (!held) {
            QTimer::singleShot(0, this, [this] {
                if (isFinished())
                    return;
                emit readyRead();
                if (!isFinished()) {
                    setFinished(true);
                    emit finished();
                }
            });
        }
    }
    void abort() override {
        if (isFinished())
            return;
        setError(QNetworkReply::OperationCanceledError, "Cancelled");
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override {
        return m_payload.size() - m_offset + QNetworkReply::bytesAvailable();
    }
protected:
    qint64 readData(char *data, qint64 maxSize) override {
        const qint64 count = qMin(maxSize, qint64(m_payload.size() - m_offset));
        if (count <= 0)
            return -1;
        memcpy(data, m_payload.constData() + m_offset, size_t(count));
        m_offset += count;
        return count;
    }
private:
    QByteArray m_payload;
    qsizetype m_offset = 0;
};

class FakeComicVineNetwork : public QNetworkAccessManager {
public:
    QList<QNetworkRequest> requests;
    QQueue<QByteArray> responses;
    int status = 200;
    bool held = false;
    void enqueue(const QJsonValue &results, int code = 1) {
        responses.enqueue(QJsonDocument(QJsonObject{{"status_code", code}, {"results", results}}).toJson());
    }
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        requests.append(request);
        return new FakeReply(request, responses.isEmpty() ? QByteArray() : responses.dequeue(), status, held, this);
    }
};

void comicVineResponses(FakeComicVineNetwork &network) {
    network.enqueue(QJsonArray{QJsonObject{
        {"id", 42}, {"name", "<b>A new title</b>"},
        {"issue_number", "7"}, {"volume", QJsonObject{{"id", 3}, {"name", "Demo Series"}}},
        {"cover_date", "2021-01-01"},
        {"api_detail_url", "https://untrusted.invalid/do-not-follow"}}});
    network.enqueue(QJsonObject{
        {"id", 42}, {"name", "A new title"}, {"issue_number", "7"},
        {"volume", QJsonObject{{"id", 3}, {"name", "Demo Series"}}},
        {"cover_date", "2021-01-01"},
        {"description", "<p>Example &amp; summary<img src=\"https://untrusted.invalid/image\"></p>"},
        {"person_credits", QJsonArray{QJsonObject{{"name", "Demo Writer"}, {"role", "writer, editor"}}}}});
    network.enqueue(QJsonObject{{"id", 3}, {"publisher", QJsonObject{{"name", "Demo Publisher"}}}});
}

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

int lines(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
}

// The window's item with this objectName. What a Repeater, ListView or Loader
// makes isn't a QObject child of the window, so findChild never finds it; walk
// the items instead. Popups aren't items: find those with findChild.
QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *comicRow(QQuickItem *from, const QString &path) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == "comicRow" && child->property("comic").toMap().value("path").toString() == path)
            return child;
        if (auto *found = comicRow(child, path))
            return found;
    }
    return nullptr;
}

QQuickItem *treeRow(QQuickItem *from, const QString &path) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == "comicTreeRow" && child->property("filePath").toString() == path)
            return child;
        if (auto *found = treeRow(child, path))
            return found;
    }
    return nullptr;
}

// A hyprctl that answers with response.json beside it and counts its calls.
QString fakeHyprctl(const QString &directory) {
    const QString command = directory + "/hyprctl";
    write(command, "#!/bin/bash\necho >> '" + directory.toUtf8() + "/calls'\ncat '" + directory.toUtf8()
                       + "/response.json'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return command;
}

QJsonObject readCatalog(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

QJsonObject readingStatus(const QString &directory) {
    QFile file(QDir(directory).filePath("current.json"));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// The placeholders in a text, sorted: %1 to %99, and the count (%n, %Ln).
QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder("%L?(?:[1-9][0-9]?|n)");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// Every text the sources hand to qsTr() or tr(), and whether a call passes it
// a count. It walks the code a token at a time, so a string never hides a
// comment or the other way round, C++'s adjacent literals join, and QML's
// single quotes count. A text that isn't a literal can't be in a catalog.
QMap<QString, bool> translatables(const QString &folder, QStringList *problems) {
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString code = QString::fromUtf8(file.readAll());
        qsizetype i = 0;
        auto literal = [&](QString *text) {
            const QChar quote = code[i++];
            for (; i < code.size() && code[i] != quote; ++i) {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    const QChar escaped = code[++i];
                    const int digits = escaped == 'u' ? 4 : escaped == 'x' ? 2 : 0;
                    bool hex = false;
                    const ushort unit = code.mid(i + 1, digits).toUShort(&hex, 16);
                    if (digits && hex) {
                        *text += QChar(unit);
                        i += digits;
                    } else {
                        *text += escaped == 'n' ? QChar('\n') : escaped == 't' ? QChar('\t') : escaped;
                    }
                } else {
                    *text += code[i];
                }
            }
            ++i;
        };
        auto skipSpace = [&] {
            while (i < code.size() && code[i].isSpace())
                ++i;
        };
        while (i < code.size()) {
            const QChar c = code[i];
            if (code.mid(i, 2) == "//") {
                i = code.indexOf('\n', i);
                if (i < 0)
                    break;
            } else if (code.mid(i, 2) == "/*") {
                i = code.indexOf("*/", i + 2);
                if (i < 0)
                    break;
                i += 2;
            } else if (c == '"' || c == '\'') {
                QString skipped;
                literal(&skipped);
            } else if (c.isLetter() || c == '_') {
                const qsizetype start = i;
                while (i < code.size() && (code[i].isLetterOrNumber() || code[i] == '_'))
                    ++i;
                const QString word = code.mid(start, i - start);
                skipSpace();
                if ((word != "qsTr" && word != "tr") || i >= code.size() || code[i] != '(')
                    continue;
                ++i;
                skipSpace();
                if (i >= code.size() || (code[i] != '"' && code[i] != '\'')) {
                    *problems << info.fileName() + ": not a literal: " + code.mid(start, 40);
                    continue;
                }
                QString text;
                do {
                    literal(&text);
                    skipSpace();
                } while (i < code.size() && code[i] == '"');
                // The arguments after the text, to the call's closing parenthesis.
                int depth = 0, commas = 0;
                while (i < code.size()) {
                    const QChar d = code[i];
                    if (d == '"' || d == '\'') {
                        QString skipped;
                        literal(&skipped);
                        continue;
                    }
                    if (d == '(' || d == '[' || d == '{')
                        ++depth;
                    else if ((d == ')' || d == ']' || d == '}') && depth-- == 0)
                        break;
                    else if (d == ',' && depth == 0)
                        ++commas;
                    ++i;
                }
                found[text] = found.value(text) || commas >= 2;
            } else {
                ++i;
            }
        }
    }
    return found;
}

} // namespace

class OmaComicsTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void buildsTreeWithoutResettingForMetadata() {
        ComicTreeModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVariantList books{
            QVariantMap{{"path", "/library/series/one.cbz"}, {"title", "One"}},
            QVariantMap{{"path", "/library/series/sub/two.cbz"}, {"title", "Two"}},
            QVariantMap{{"path", "/library/other/three.cbz"}, {"title", "Three"}}};
        const QVariantList roots{"/library", "/library/series"};
        model.synchronize(books, roots);
        QCOMPARE(model.rowCount(), 1);
        const auto root = model.index(0, 0);
        QCOMPARE(model.data(root, Qt::DisplayRole).toString(), QString("library"));
        QCOMPARE(model.rowCount(root), 2);
        const auto series = model.index(1, 0, root);
        QCOMPARE(model.data(series, Qt::DisplayRole).toString(), QString("series"));
        const auto first = model.index(0, 0, series);
        QCOMPARE(model.data(first, Qt::DisplayRole).toString(), QString("One"));
        QCOMPARE(model.parent(first), series);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        auto updated = books.first().toMap();
        updated.insert("title", "A different title");
        books[0] = updated;
        model.synchronize(books, roots);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(model.data(first, Qt::DisplayRole).toString(), QString("A different title"));
        model.synchronize({books[1]}, roots);
        QCOMPARE(resets.size(), 1);
        QCOMPARE(model.rowCount(model.index(0, 0)), 1);
    }

    void selectsFoldersRecursivelyWithFilters() {
        QTemporaryDir data, cache;
        const QJsonArray books{
            QJsonObject{{"path", "/library/series/one.cbz"}, {"title", "One"}, {"pageCount", 1}},
            QJsonObject{{"path", "/library/series/sub/two.cbz"}, {"title", "Two"}, {"pageCount", 1}},
            QJsonObject{{"path", "/library/series-extra/three.cbz"}, {"title", "Three"}, {"pageCount", 1}}};
        QVERIFY(write(data.filePath("library.json"),
                      QJsonDocument(QJsonObject{{"books", books}}).toJson()));
        Backend backend(data.path(), cache.path());
        auto *model = backend.comicTreeModel();
        const int selectionRole = model->roleNames().key("selectionState");
        const auto root = model->index(0, 0);
        const auto series = model->index(1, 0, root);
        QCOMPARE(model->data(series, Qt::DisplayRole).toString(), QString("series"));
        QSignalSpy changes(&backend, &Backend::selectionChanged);
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        backend.selectFolder("/library/series/", true);
        QCOMPARE(backend.selectedComics().size(), 2);
        QVERIFY(backend.selectedComics().contains("/library/series/sub/two.cbz"));
        QVERIFY(!backend.selectedComics().contains("/library/series-extra/three.cbz"));
        QCOMPARE(changes.size(), 1);
        QCOMPARE(model->data(series, selectionRole).toInt(), int(Qt::Checked));
        QCOMPARE(model->data(root, selectionRole).toInt(), int(Qt::PartiallyChecked));
        backend.selectFolder("/library/series", true);
        QCOMPARE(changes.size(), 1);
        backend.selectComic("/library/series/one.cbz", false);
        QCOMPARE(model->data(series, selectionRole).toInt(), int(Qt::PartiallyChecked));
        backend.selectFolder("/library", true);
        QCOMPARE(model->data(root, selectionRole).toInt(), int(Qt::Checked));
        backend.selectFolder("/library/series", false);
        QCOMPARE(backend.selectedComics(), QStringList{"/library/series-extra/three.cbz"});
        QCOMPARE(model->data(series, selectionRole).toInt(), int(Qt::Unchecked));
        QCOMPARE(resets.size(), 0);
        backend.clearSelection();
        QCOMPARE(model->data(root, selectionRole).toInt(), int(Qt::Unchecked));
        backend.setSearchText("Two");
        backend.selectFolder("/library", true);
        QCOMPARE(backend.selectedComics(), QStringList{"/library/series/sub/two.cbz"});
        backend.setSearchText("");
        QCOMPARE(model->data(model->index(0, 0), selectionRole).toInt(), int(Qt::PartiallyChecked));
        backend.selectFolder("/library", false);
        QVERIFY(backend.selectedComics().isEmpty());
    }

    void reviewsMetadataAndPreservesLocalDetails() {
        QTemporaryDir data, cache;
        const QJsonObject book{{"path", "/comics/filename.cbz"}, {"title", "filename"},
                               {"writer", "Embedded Writer"}, {"progress", 4}, {"pageCount", 12},
                               {"bookmarks", QJsonArray{3}}, {"collection", "Favorites"}};
        QVERIFY(write(data.filePath("library.json"), QJsonDocument(QJsonObject{{"books", QJsonArray{book}}}).toJson()));
        FakeComicVineNetwork network;
        comicVineResponses(network);
        Backend backend(data.path(), cache.path(), nullptr, &network);
        auto *scraper = backend.scraper();
        scraper->search("Demo Series 7");
        QVERIFY(!scraper->message().isEmpty());
        QCOMPARE(network.requests.size(), 0);
        scraper->setApiKey(QString(40, 'a'));
        scraper->search("Demo & Series 7");
        QTRY_VERIFY(!scraper->busy());
        QCOMPARE(scraper->matches().size(), 1);
        QCOMPARE(scraper->matches().first().toMap().value("title").toString(), QString("A new title"));
        const QUrl url = network.requests.first().url();
        QCOMPARE(url.scheme(), QString("https"));
        QCOMPARE(url.host(), QString("comicvine.gamespot.com"));
        QCOMPARE(QUrlQuery(url).queryItemValue("query"), QString("Demo & Series 7"));
        scraper->choose(42);
        QTRY_VERIFY(!scraper->busy());
        QCOMPARE(scraper->metadata().value("summary").toString(), QString("Example & summary"));
        QCOMPARE(network.requests.size(), 3);
        QCOMPARE(backend.comics().first().toMap().value("title").toString(), QString("filename"));
        QCOMPARE(backend.missingMetadata("/comics/filename.cbz").size(), 6);
        QVERIFY(backend.applyScrapedMetadata("/comics/filename.cbz"));
        const auto updated = backend.comics().first().toMap();
        QCOMPARE(updated.value("title").toString(), QString("A new title"));
        QCOMPARE(updated.value("writer").toString(), QString("Embedded Writer"));
        QCOMPARE(updated.value("publisher").toString(), QString("Demo Publisher"));
        QCOMPARE(updated.value("progress").toInt(), 4);
        QCOMPARE(updated.value("bookmarks").toList(), QVariantList{3});
        QCOMPARE(updated.value("collection").toString(), QString("Favorites"));
        Backend restored(data.path(), cache.path());
        QCOMPARE(restored.comics().first().toMap().value("title").toString(), QString("A new title"));
        QVERIFY(!restored.scraper()->configured());
        QFile saved(data.filePath("library.json"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(!saved.readAll().contains(QString(40, 'a').toLatin1()));
        QCOMPARE(backend.missingMetadata("/comics/filename.cbz").size(), 0);
        scraper->choose(999);
        QVERIFY(!scraper->message().isEmpty());
        QCOMPARE(network.requests.size(), 3);
        scraper->search("Demo & Series 7");
        QVERIFY(!scraper->busy());
        scraper->choose(42);
        QVERIFY(!scraper->busy());
        QCOMPARE(network.requests.size(), 3);
        scraper->search("Different query");
        QVERIFY(scraper->busy());
        QCOMPARE(network.requests.size(), 3);
        scraper->cancel();
        QVERIFY(!scraper->busy());
        QVERIFY(scraper->metadata().isEmpty());
        for (const auto &request : std::as_const(network.requests)) {
            QCOMPARE(request.url().host(), QString("comicvine.gamespot.com"));
            QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                     int(QNetworkRequest::ManualRedirectPolicy));
        }
    }

    void handlesComicVineErrors_data() {
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QString>("message");
        QTest::newRow("invalid-json") << QByteArray("not json") << 200 << QString("invalid response");
        QTest::newRow("bad-key") << QByteArray("{\"status_code\":100}") << 200 << QString("rejected");
        QTest::newRow("malformed-results") << QByteArray("{\"status_code\":1,\"results\":{}}")
                                         << 200 << QString("invalid response");
        QTest::newRow("invalid-identifier") << QByteArray("{\"status_code\":1,\"results\":[{\"id\":-1}]}")
                                           << 200 << QString("invalid response");
        QTest::newRow("large-response") << QByteArray(2 * 1024 * 1024 + 1, 'x') << 200 << QString("too much data");
        QTest::newRow("rate-limit") << QByteArray() << 429 << QString("rate limiting");
        QTest::newRow("redirect") << QByteArray() << 302 << QString("request failed");
    }

    void handlesComicVineErrors() {
        QFETCH(QByteArray, response);
        QFETCH(int, httpStatus);
        QFETCH(QString, message);
        FakeComicVineNetwork network;
        network.responses.enqueue(response);
        network.status = httpStatus;
        ComicVine scraper(nullptr, &network);
        scraper.setApiKey(QString(40, 'a'));
        scraper.search("Example");
        QTRY_VERIFY(!scraper.busy());
        QVERIFY2(scraper.message().contains(message), qPrintable(scraper.message()));
        QVERIFY(scraper.metadata().isEmpty());
        QVERIFY(scraper.matches().isEmpty());
        QCOMPARE(network.requests.size(), 1);
    }

    void cancelsComicVineRequests() {
        FakeComicVineNetwork network;
        network.held = true;
        ComicVine scraper(nullptr, &network);
        scraper.setApiKey(QString(40, 'a'));
        scraper.search("Example");
        QVERIFY(scraper.busy());
        QCOMPARE(network.requests.size(), 1);
        scraper.cancel();
        QCoreApplication::processEvents();
        QVERIFY(!scraper.busy());
        QVERIFY(scraper.message().isEmpty());
        QVERIFY(scraper.matches().isEmpty());
    }

    void retainsScrapedMetadataAfterReindexing() {
        QTemporaryDir data, cache, library;
        QImage page(8, 8, QImage::Format_RGB32);
        page.fill(Qt::red);
        QVERIFY(page.save(library.filePath("1.png")));
        const auto archive = [&](const QString &entry) {
            QProcess process;
            process.setWorkingDirectory(library.path());
            process.start("7z", {"a", "-tzip", library.filePath("sample.cbz"), entry});
            return process.waitForStarted() && process.waitForFinished(10000) && process.exitCode() == 0;
        };
        QVERIFY(archive("1.png"));
        QFile comic(library.filePath("sample.cbz"));
        QVERIFY(comic.open(QIODevice::ReadOnly));
        const QByteArray original = comic.readAll();
        comic.close();
        FakeComicVineNetwork network;
        comicVineResponses(network);
        Backend backend(data.path(), cache.path(), nullptr, &network);
        backend.addLibraryFolder(library.path());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        backend.scraper()->setApiKey(QString(40, 'a'));
        backend.scraper()->search("Demo Series 7");
        QTRY_VERIFY(!backend.scraper()->busy());
        backend.scraper()->choose(42);
        QTRY_VERIFY(!backend.scraper()->busy());
        QVERIFY(backend.applyScrapedMetadata(comic.fileName()));
        QCOMPARE(backend.comics().first().toMap().value("title").toString(), QString("A new title"));
        QVERIFY(comic.open(QIODevice::ReadOnly));
        QCOMPARE(comic.readAll(), original);
        comic.close();
        QVERIFY(write(library.filePath("ComicInfo.xml"),
                      "<ComicInfo><Writer>Embedded Writer</Writer><Summary>Embedded summary</Summary></ComicInfo>"));
        QVERIFY(archive("ComicInfo.xml"));
        backend.scanLibrary();
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        const auto updated = backend.comics().first().toMap();
        QCOMPARE(updated.value("title").toString(), QString("A new title"));
        QCOMPARE(updated.value("writer").toString(), QString("Embedded Writer"));
        QCOMPARE(updated.value("summary").toString(), QString("Embedded summary"));
        QCOMPARE(updated.value("publisher").toString(), QString("Demo Publisher"));
        Backend reopened(data.path(), cache.path());
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.scanning(), 10000);
        QCOMPARE(reopened.comics().first().toMap().value("title").toString(), QString("A new title"));
    }

    void doesNotApplyMetadataWhenSavingFails() {
        QTemporaryDir data, cache;
        QVERIFY(write(data.filePath("library.json"),
                      R"({"books":[{"path":"/comic.cbz","title":"comic","pageCount":1}]})"));
        FakeComicVineNetwork network;
        comicVineResponses(network);
        Backend backend(data.path(), cache.path(), nullptr, &network);
        backend.scraper()->setApiKey(QString(40, 'a'));
        backend.scraper()->search("Demo");
        QTRY_VERIFY(!backend.scraper()->busy());
        backend.scraper()->choose(42);
        QTRY_VERIFY(!backend.scraper()->busy());
        QVERIFY(QFile::remove(data.filePath("library.json")));
        QVERIFY(QDir().mkpath(data.filePath("library.json")));
        QVERIFY(!backend.applyScrapedMetadata("/comic.cbz"));
        QCOMPARE(backend.comics().first().toMap().value("title").toString(), QString("comic"));
        QVERIFY(backend.status().startsWith("The library could not be saved:"));
    }

    void browsesTreeAndReviewsComicVine_data() {
        QTest::addColumn<QString>("language");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::newRow("default") << QString("en") << 1160 << 760;
        QTest::newRow("danish") << QString("da") << 1160 << 760;
        QTest::newRow("rtl") << QString("ar") << 1160 << 760;
        QTest::newRow("compact") << QString("en") << 820 << 560;
    }

    void browsesTreeAndReviewsComicVine() {
        QFETCH(QString, language);
        QFETCH(int, width);
        QFETCH(int, height);
        QTemporaryDir data, cache;
        const QJsonArray books{
            QJsonObject{{"path", "/library/series/one.cbz"}, {"title", "one"}, {"pageCount", 1}, {"format", "CBZ"}},
            QJsonObject{{"path", "/library/series/two.cbz"}, {"title", "two"}, {"pageCount", 1}, {"format", "CBZ"}}};
        QVERIFY(write(data.filePath("library.json"),
                      QJsonDocument(QJsonObject{{"books", books}, {"thumbnailSize", 0}}).toJson()));
        QVERIFY(write(data.filePath("language"), language.toUtf8()));
        Translator translator(data.filePath("language"));
        QCoreApplication::installTranslator(&translator);
        const QString colors = qEnvironmentVariable("OMACOMICS_TEST_COLORS");
        if (!colors.isEmpty()) {
            QVERIFY(QDir().mkpath(data.filePath("theme")));
            QVERIFY(QFile::copy(colors, data.filePath("theme/colors.toml")));
        }
        Theme theme(data.path());
        FakeComicVineNetwork network;
        comicVineResponses(network);
        Backend backend(data.path(), cache.path(), nullptr, &network);
        FolderPicker folderPicker;
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("folderPicker", &folderPicker);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->resize(width, height);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        const auto click = [window](QQuickItem *control) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              control->mapToScene(QPointF(control->width() / 2, control->height() / 2)).toPoint());
        };
        const auto snapshot = [window](const QString &name) {
            const QString directory = qEnvironmentVariable("OMACOMICS_TEST_SCREENSHOTS");
            if (directory.isEmpty())
                return true;
            QTest::qWait(250);
            return window->grabWindow().save(QDir(directory).filePath(QString::fromLatin1(QTest::currentDataTag())
                                                                    + "-" + name + ".png"));
        };
        QVERIFY(snapshot("list"));
        auto *listButton = item(window->contentItem(), "listViewButton");
        auto *treeButton = item(window->contentItem(), "treeViewButton");
        auto *folderLabel = item(window->contentItem(), "libraryFoldersLabel");
        auto *actionGroup = item(window->contentItem(), "libraryActionGroup");
        auto *tree = item(window->contentItem(), "comicTree");
        auto *list = item(window->contentItem(), "bookList");
        QVERIFY(listButton);
        QVERIFY(treeButton);
        QVERIFY(folderLabel);
        QVERIFY(actionGroup);
        QVERIFY(tree);
        QVERIFY(list);
        QTRY_VERIFY(qAbs(folderLabel->mapToScene(QPointF(0, folderLabel->height() / 2)).y()
                        - actionGroup->mapToScene(QPointF(0, actionGroup->height() / 2)).y()) < 1);
        if (language == "ar")
            QVERIFY(actionGroup->mapToScene(QPointF(actionGroup->width(), 0)).x()
                    <= folderLabel->mapToScene(QPointF()).x());
        else
            QVERIFY(actionGroup->mapToScene(QPointF()).x()
                    >= folderLabel->mapToScene(QPointF(folderLabel->width(), 0)).x());
        QVERIFY(listButton->property("checked").toBool());
        click(treeButton);
        QTRY_VERIFY(backend.treeView());
        QVERIFY(treeButton->property("checked").toBool());
        QVERIFY(!listButton->property("checked").toBool());
        QVERIFY(tree->isVisible());
        QVERIFY(!list->isVisible());
        QQuickItem *root = nullptr;
        QTRY_VERIFY((root = treeRow(tree, "/library")));
        auto *rootCheck = item(root, "treeComicCheck");
        QVERIFY(rootCheck);
        click(rootCheck);
        QCOMPARE(backend.selectedComics().size(), 2);
        QCOMPARE(rootCheck->property("checkState").toInt(), int(Qt::Checked));
        QVERIFY(snapshot("folder-selected"));
        click(rootCheck);
        QVERIFY(backend.selectedComics().isEmpty());
        click(item(root, "treeComicLabel"));
        QQuickItem *series = nullptr;
        QTRY_VERIFY((series = treeRow(tree, "/library/series")));
        auto *seriesCheck = item(series, "treeComicCheck");
        QVERIFY(seriesCheck);
        click(item(series, "treeComicLabel"));
        QQuickItem *first = nullptr;
        QQuickItem *second = nullptr;
        QTRY_VERIFY((first = treeRow(tree, "/library/series/one.cbz")));
        QTRY_VERIFY((second = treeRow(tree, "/library/series/two.cbz")));
        QCOMPARE(item(first, "treeComicLabel")->property("effectiveHorizontalAlignment").toInt(),
                 language == "ar" ? int(Qt::AlignRight) : int(Qt::AlignLeft));
        QVERIFY(snapshot("tree"));
        auto *firstCheck = item(first, "treeComicCheck");
        auto *secondCheck = item(second, "treeComicCheck");
        QVERIFY(firstCheck);
        QVERIFY(secondCheck);
        click(firstCheck);
        QCOMPARE(seriesCheck->property("checkState").toInt(), int(Qt::PartiallyChecked));
        QCOMPARE(rootCheck->property("checkState").toInt(), int(Qt::PartiallyChecked));
        QVERIFY(snapshot("folder-partial"));
        click(seriesCheck);
        QCOMPARE(backend.selectedComics().size(), 2);
        QVERIFY(secondCheck->property("checked").toBool());
        QCOMPARE(seriesCheck->property("checkState").toInt(), int(Qt::Checked));
        click(secondCheck);
        QCOMPARE(seriesCheck->property("checkState").toInt(), int(Qt::PartiallyChecked));
        click(seriesCheck);
        QCOMPARE(backend.selectedComics().size(), 2);
        auto *selectionMenuButton = item(window->contentItem(), "selectionMenuButton");
        QVERIFY(selectionMenuButton);
        click(selectionMenuButton);
        auto *selectionMenu = window->findChild<QObject *>("selectionMenu");
        QVERIFY(selectionMenu);
        QTRY_VERIFY(selectionMenu->property("opened").toBool());
        auto *mangaOption = item(window->contentItem(), "selectedMangaOption");
        QVERIFY(mangaOption);
        click(mangaOption);
        QTRY_VERIFY(!selectionMenu->property("visible").toBool());
        QVERIFY(backend.comics().first().toMap().value("manga").toBool());
        QVERIFY(backend.comics().last().toMap().value("manga").toBool());
        listButton->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!backend.treeView());
        QVERIFY(listButton->property("checked").toBool());
        QVERIFY(!treeButton->property("checked").toBool());
        QVERIFY(list->isVisible());
        auto *firstList = comicRow(list, "/library/series/one.cbz");
        QVERIFY(firstList);
        QVERIFY(item(firstList, "selectComicCheck")->property("checked").toBool());
        click(item(window->contentItem(), "scrapeSelectedButton"));
        auto *dialog = window->findChild<QObject *>("scrapeDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(snapshot("key"));
        QCOMPARE(network.requests.size(), 0);
        auto *apiKey = item(window->contentItem(), "comicVineApiKey");
        QVERIFY(apiKey);
        apiKey->setProperty("text", QString(40, 'a'));
        click(item(window->contentItem(), "setComicVineKey"));
        QVERIFY(backend.scraper()->configured());
        QCOMPARE(apiKey->property("text").toString(), QString());
        click(item(window->contentItem(), "searchComicVine"));
        QTRY_VERIFY(!backend.scraper()->busy());
        auto *match = item(window->contentItem(), "comicVineMatch");
        QTRY_VERIFY((match = item(window->contentItem(), "comicVineMatch")));
        QTRY_VERIFY(match->width() > 0 && match->height() > 0);
        auto *resultList = item(window->contentItem(), "comicVineMatches");
        QVERIFY(resultList);
        QTRY_VERIFY(resultList->height() >= match->height());
        QTest::qWait(50);
        QVERIFY(snapshot("matches"));
        click(match);
        QTRY_VERIFY(!backend.scraper()->busy());
        auto *apply = item(window->contentItem(), "applyComicVine");
        QVERIFY(apply);
        QTRY_VERIFY(apply->isEnabled());
        QVERIFY(snapshot("review"));
        QCOMPARE(backend.comics().first().toMap().value("title").toString(), QString("one"));
        click(apply);
        QTRY_COMPARE(dialog->property("position").toInt(), 1);
        QCOMPARE(backend.comics().first().toMap().value("title").toString(), QString("A new title"));
        QCOMPARE(backend.comics().last().toMap().value("title").toString(), QString("two"));
        click(item(window->contentItem(), "skipComicVine"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        backend.setSearchText("A new title");
        QCOMPARE(backend.selectedComics(), QStringList{"/library/series/one.cbz"});
        backend.clearSelection();
        QVERIFY(backend.selectedComics().isEmpty());
        backend.setSearchText("");
        backend.selectAllVisible();
        QCOMPARE(backend.selectedComics().size(), 2);
        backend.setTreeView(true);
        Backend restored(data.path(), cache.path());
        QVERIFY(restored.treeView());
        QVERIFY(restored.comics().first().toMap().value("manga").toBool());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
        QLocale::setDefault(QLocale("en"));
    }

    void updatesComicModelWithoutResetting() {
        ComicListModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        const auto book = [](const QString &path, const QString &title) {
            return QVariantMap{{"path", path}, {"title", title}};
        };
        model.synchronize({book("a", "Alpha"), book("b", "Beta"), book("c", "Gamma")});
        QCOMPARE(model.rowCount(), 3);
        const QPersistentModelIndex gamma(model.index(2, 0));
        model.synchronize({book("c", "Updated"), book("a", "Alpha"), book("d", "Delta")});
        QCOMPARE(model.rowCount(), 3);
        QVERIFY(gamma.isValid());
        QCOMPARE(gamma.row(), 0);
        const int role = model.roleNames().key("comic");
        QCOMPARE(model.data(model.index(0, 0), role).toMap().value("title").toString(), QString("Updated"));
        QCOMPARE(model.data(model.index(1, 0), role).toMap().value("path").toString(), QString("a"));
        QCOMPARE(model.data(model.index(2, 0), role).toMap().value("path").toString(), QString("d"));
        QCOMPARE(resets.size(), 0);
    }

    void librarySearchCollectionsAndSettings() {
        QTemporaryDir data, cache;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(write(data.filePath("library.json"),
                      R"({"folders":[],"current":"","thumbnailSize":88,"books":[)"
                      R"({"path":"/comics/space.cbz","title":"Space Saga","series":"Saga","collection":"Sci-Fi","pageCount":20},)"
                      R"({"path":"/comics/moon.pdf","title":"Moon Notes","collection":"Fantasy","pageCount":4}]})"));

        Backend backend(data.path(), cache.path());
        QCOMPARE(backend.comics().size(), 2);
        backend.setSearchText("saga");
        QCOMPARE(backend.visibleComics().size(), 1);
        QCOMPARE(backend.visibleComics().first().toMap().value("title").toString(), QString("Space Saga"));
        backend.setCollectionFilter("Fantasy");
        QVERIFY(backend.visibleComics().isEmpty());
        backend.setSearchText("");
        QCOMPARE(backend.visibleComics().size(), 1);

        backend.setThumbnailSize(999);
        QCOMPARE(backend.thumbnailSize(), 192);
        backend.setThumbnailSize(1);
        QCOMPARE(backend.thumbnailSize(), 48);
        backend.setThumbnailSize(0);
        QCOMPARE(backend.thumbnailSize(), 0);
        backend.setCollection("/comics/space.cbz", "Favorites");
        QCOMPARE(backend.collections().size(), 2);
        backend.setArrowNavigation(false);
        backend.setWheelNavigation(false);
        backend.setMouseNavigation(false);

        QFile saved(data.filePath("library.json"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(saved.readAll()).object();
        QCOMPARE(root.value("thumbnailSize").toInt(), 0);
        QCOMPARE(root.value("books").toArray().first().toObject().value("collection").toString(),
                 QString("Favorites"));
        Backend restored(data.path(), cache.path());
        QCOMPARE(restored.thumbnailSize(), 0);
        QVERIFY(!restored.arrowNavigation());
        QVERIFY(!restored.wheelNavigation());
        QVERIFY(!restored.mouseNavigation());
    }

    void searchTermsMatchTogether() {
        QTemporaryDir data, cache;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(write(data.filePath("library.json"),
                      R"({"folders":[],"books":[)"
                      R"({"path":"/a.cbz","title":"Space Saga","writer":"Alice Smith","collection":"Favorites"},)"
                      R"({"path":"/b.cbz","title":"Space Odyssey","writer":"Bob Smith","collection":"Other"},)"
                      R"({"path":"/c.cbz","title":"Moon Saga","writer":"Alice Jones","collection":"Favorites"}]})"));
        Backend backend(data.path(), cache.path());
        QSignalSpy resets(backend.visibleComicModel(), &QAbstractItemModel::modelReset);
        backend.setSearchText("  SPACE  ");
        QCOMPARE(backend.visibleComics().size(), 2);
        backend.commitSearchTerm();
        QCOMPARE(backend.searchTerms(), QStringList{"SPACE"});
        backend.setSearchText("alice smith");
        QCOMPARE(backend.visibleComics().size(), 1);
        backend.commitSearchTerm();
        QCOMPARE(backend.searchTerms(), (QStringList{"SPACE", "alice smith"}));
        QCOMPARE(backend.visibleComicModel()->rowCount(), 1);
        backend.setCollectionFilter("Other");
        QVERIFY(backend.visibleComics().isEmpty());
        backend.setCollectionFilter("Favorites");
        QCOMPARE(backend.visibleComics().size(), 1);
        backend.setCollectionFilter("");
        backend.removeSearchTerm(0);
        QCOMPARE(backend.searchTerms(), QStringList{"alice smith"});
        QCOMPARE(backend.visibleComics().size(), 1);
        backend.setSearchText(" ALICE SMITH ");
        backend.commitSearchTerm();
        QCOMPARE(backend.searchTerms(), QStringList{"alice smith"});
        backend.setSearchText("   ");
        backend.commitSearchTerm();
        QCOMPARE(backend.searchTerms(), QStringList{"alice smith"});
        backend.removeSearchTerm(-1);
        backend.removeSearchTerm(1);
        QCOMPARE(backend.searchTerms(), QStringList{"alice smith"});
        backend.setSearchText("");
        backend.removeSearchTerm(0);
        QCOMPARE(backend.visibleComics().size(), 3);
        QCOMPARE(resets.size(), 0);
    }

    void persistsAllPreferencesAndRejectsInvalidChanges() {
        QTemporaryDir data, cache;
        Backend backend(data.path(), cache.path());
        QVERIFY(backend.arrowNavigation());
        QVERIFY(backend.wheelNavigation());
        QVERIFY(backend.mouseNavigation());
        const QVariantMap changes{{"backupRetentionDays", 90}, {"startupBehavior", 1},
                                  {"scanOnStartup", false}, {"fitMode", 2}, {"zoomStep", 6},
                                  {"libraryVisible", false}, {"thumbnailSize", 0}, {"doublePage", true},
                                  {"treeView", true}, {"arrowNavigation", false}, {"wheelNavigation", false},
                                  {"mouseNavigation", false}, {"rememberApiKey", false}, {"useSeriesForLookup", false}};
        for (auto it = changes.begin(); it != changes.end(); ++it)
            QVERIFY(backend.setPreference(it.key(), it.value()));
        Backend reopened(data.path(), cache.path());
        for (auto it = changes.begin(); it != changes.end(); ++it)
            QCOMPARE(reopened.preferences().value(it.key()), it.value());
        QVERIFY(!backend.setPreference("rightToLeft", true));
        QVERIFY(!backend.setPreference("backupRetentionDays", 31));
        QVERIFY(!backend.setPreference("zoomStep", QString("7")));
        QVERIFY(!backend.setPreference("scanOnStartup", 1));
        QVERIFY(write(data.filePath("restore.json"),
                      R"({"folders":[],"books":[],"doublePage":false,"treeView":false})"));
        reopened.restoreLibrary(data.filePath("restore.json"));
        QVERIFY(reopened.doublePage());
        QVERIFY(reopened.treeView());
        QCOMPARE(reopened.preferences().value("zoomStep").toInt(), 6);
        QVERIFY(QFile::remove(data.filePath("settings.json")));
        QVERIFY(QDir().mkpath(data.filePath("settings.json")));
        QVERIFY(!reopened.setPreference("fitMode", 0));
        QCOMPARE(reopened.preferences().value("fitMode").toInt(), 2);
    }

    void prunesOnlyExpiredAutomaticBackups_data() {
        QTest::addColumn<int>("days");
        QTest::newRow("week") << 7;
        QTest::newRow("month") << 30;
        QTest::newRow("quarter") << 90;
    }

    void prunesOnlyExpiredAutomaticBackups() {
        QFETCH(int, days);
        QTemporaryDir data, cache, external;
        QVERIFY(write(data.filePath("library.json"),
                      R"({"folders":[],"books":[{"path":"/comic.cbz","title":"Comic"}]})"));
        Backend backend(data.path(), cache.path());
        QTRY_VERIFY(!backend.backupCleanupBusy());
        QVERIFY(backend.setPreference("backupRetentionDays", 0));
        const QDir backups(backend.backupDirectory());
        const QByteArray snapshot = R"({"books":[],"folders":[]})";
        const auto daily = [&](int age) {
            return backups.filePath("daily-" + QDate::currentDate().addDays(-age).toString(Qt::ISODate) + ".json");
        };
        const QString expired = daily(days);
        const QString boundary = daily(days - 1);
        const QString symlink = daily(days + 1);
        const QString manual = backups.filePath("omacomics-20000101-000000-000-" + QString(32, '0') + ".json");
        const QString safety = backups.filePath("safety-20000101-000000-000-" + QString(32, '0') + ".json");
        const QString malformed = backups.filePath("daily-2000-99-99.json");
        const QString unrelated = backups.filePath("daily-2000-01-02.json");
        const QString directory = daily(days + 2);
        QVERIFY(write(expired, snapshot));
        QVERIFY(write(boundary, snapshot));
        QVERIFY(write(manual, snapshot));
        QVERIFY(write(safety, snapshot));
        QVERIFY(write(malformed, snapshot));
        QVERIFY(write(unrelated, R"({"unrelated":true})"));
        QVERIFY(write(external.filePath("source.json"), snapshot));
        QVERIFY(QFile::link(external.filePath("source.json"), symlink));
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(backend.setPreference("backupRetentionDays", days));
        QTRY_VERIFY(!QFileInfo::exists(expired));
        QTRY_VERIFY(!QFileInfo::exists(safety));
        QTRY_VERIFY(!backend.backupCleanupBusy());
        for (const QString &path : {boundary, manual, malformed, unrelated, symlink, directory, external.filePath("source.json")})
            QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        QVERIFY(backend.setPreference("backupRetentionDays", 0));
        QVERIFY(write(expired, snapshot));
        backend.setCollection("/comic.cbz", "Unpruned");
        QTest::qWait(50);
        QVERIFY(QFileInfo::exists(expired));
    }

    void clearsOnlyGeneratedImagesAndPreservesLibrary() {
        QTemporaryDir data, cache, external;
        const QByteArray library = R"({"folders":[],"books":[{"path":"/one.cbz","title":"One","progress":4,"bookmarks":[2]}]})";
        QVERIFY(write(data.filePath("library.json"), library));
        Backend backend(data.path(), cache.path());
        const QString cover = cache.filePath("covers/" + QString(64, 'a') + ".png");
        const QString page = cache.filePath("pages/" + QString(64, 'b') + ".jpg");
        const QString index = cache.filePath("indexes/page-index.json");
        const QString notes = cache.filePath("covers/notes.txt");
        const QString link = cache.filePath("pages/" + QString(64, 'c') + ".png");
        QVERIFY(write(cover, "cover"));
        QVERIFY(write(page, "page"));
        QVERIFY(write(index, "[\"one.png\"]"));
        QVERIFY(write(notes, "not generated"));
        QVERIFY(write(external.filePath("keep.png"), "original"));
        QVERIFY(QFile::link(external.filePath("keep.png"), link));
        backend.refreshCacheSize();
        QTRY_VERIFY(!backend.cacheBusy());
        QCOMPARE(backend.cacheBytes(), 9);
        backend.clearImageCache();
        QTRY_VERIFY(!backend.cacheBusy());
        QTRY_VERIFY(backend.status().startsWith("Image cache cleared."));
        QVERIFY(!QFileInfo::exists(cover));
        QVERIFY(!QFileInfo::exists(page));
        for (const QString &path : {index, notes, link, external.filePath("keep.png")})
            QVERIFY(QFileInfo::exists(path));
        QCOMPARE(backend.cacheBytes(), 0);
        QCOMPARE(backend.comics().first().toMap().value("progress").toInt(), 4);
        QCOMPARE(backend.comics().first().toMap().value("bookmarks").toList(), QVariantList{2});
        QFile saved(data.filePath("library.json"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), library);
    }

    void remembersKeysOnlyThroughCredentialStorage() {
        QTemporaryDir data, cache, tool;
        const QString vault = tool.filePath("test-vault");
        const QString arguments = tool.filePath("arguments");
        const QString executable = tool.filePath("secret-tool");
        const QByteArray script = "#!/bin/bash\ncase \"$1\" in\n"
            "store) cat > '" + vault.toUtf8() + "'; printf '%s\\n' \"$@\" > '" + arguments.toUtf8() + "';;\n"
            "lookup) if [[ -f '" + vault.toUtf8() + "' ]]; then cat '" + vault.toUtf8() + "'; else exit 1; fi;;\n"
            "clear) rm -- '" + vault.toUtf8() + "';;\nesac\n";
        QVERIFY(write(executable, script));
        QVERIFY(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        Backend backend(data.path(), cache.path(), nullptr, nullptr, executable);
        const QString key(40, 'a');
        backend.setComicVineKey(key);
        QTRY_VERIFY(!backend.credentials()->busy());
        QVERIFY(backend.credentials()->message().contains("saved in desktop"));
        QVERIFY(backend.scraper()->configured());
        QFile args(arguments);
        QVERIFY(args.open(QIODevice::ReadOnly));
        QVERIFY(!args.readAll().contains(key.toUtf8()));
        QVERIFY(backend.setPreference("fitMode", 1));
        backend.backupLibrary();
        QDirIterator files(data.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(!file.readAll().contains(key.toUtf8()));
        }
        Backend reopened(data.path(), cache.path(), nullptr, nullptr, executable);
        reopened.loadComicVineKey();
        QTRY_VERIFY(!reopened.credentials()->busy());
        QVERIFY(reopened.scraper()->configured());
        QVERIFY(reopened.setPreference("rememberApiKey", false));
        Backend sessionOnly(data.path(), cache.path(), nullptr, nullptr, executable);
        sessionOnly.loadComicVineKey();
        QVERIFY(!sessionOnly.scraper()->configured());
        reopened.forgetComicVineKey();
        QTRY_VERIFY(!reopened.credentials()->busy());
        QVERIFY(!QFileInfo::exists(vault));
        QVERIFY(!reopened.scraper()->configured());
        CredentialStore missing(QString{});
        missing.store(key);
        QVERIFY(missing.message().contains("session-only"));
    }

    void reportsCredentialFailuresWithoutLoadingInvalidKeys() {
        QTemporaryDir dir;
        const QString executable = dir.filePath("secret-tool");
        QVERIFY(write(executable, "#!/bin/bash\nprintf 'invalid key!'\n"));
        QVERIFY(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        CredentialStore store(executable);
        QSignalSpy loaded(&store, &CredentialStore::loaded);
        store.load();
        QTRY_VERIFY(!store.busy());
        QCOMPARE(loaded.size(), 0);
        QVERIFY(store.message().contains("failed"));
        QVERIFY(write(executable, "#!/bin/bash\nprintf 'denied' >&2\nexit 2\n"));
        store.store(QString(40, 'a'));
        QTRY_VERIFY(!store.busy());
        QVERIFY(store.message().contains("failed"));
        CredentialStore unavailable(dir.filePath("missing"));
        unavailable.load();
        QTRY_VERIFY(!unavailable.busy());
        QVERIFY(unavailable.message().contains("failed"));
    }

    void startsUpAndReadsDuringBackgroundIndexing() {
        QTemporaryDir data, cache, library, tool;
        QImage page(8, 8, QImage::Format_RGB32);
        page.fill(Qt::red);
        QVERIFY(page.save(library.filePath("1.png")));
        QVERIFY(page.save(library.filePath("2.png")));
        QProcess archive;
        archive.setWorkingDirectory(library.path());
        archive.start("7z", {"a", "-tzip", library.filePath("one.cbz"), "1.png", "2.png"});
        QVERIFY(archive.waitForStarted());
        QVERIFY(archive.waitForFinished(10000));
        QCOMPARE(archive.exitCode(), 0);
        QVERIFY(QFile::copy(library.filePath("one.cbz"), library.filePath("two.cbz")));
        Backend backend(data.path(), cache.path());
        QVERIFY(backend.setPreference("scanOnStartup", false));
        backend.addLibraryFolder(library.path());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        backend.openComic(library.filePath("one.cbz"));
        QTRY_VERIFY(!backend.pagePath().isEmpty());
        backend.setRightToLeft(true);
        QVERIFY(backend.rightToLeft());
        backend.openComic(library.filePath("two.cbz"));
        QVERIFY(!backend.rightToLeft());
        backend.selectFolder(library.path(), true);
        backend.setSelectedManga(true);
        QVERIFY(backend.rightToLeft());
        backend.openComic(library.filePath("one.cbz"));
        QVERIFY(backend.rightToLeft());
        backend.clearImageCache();
        QVERIFY(backend.status().contains("Close the reader"));
        const QByteArray oldPath = qgetenv("PATH");
        const auto restorePath = qScopeGuard([oldPath] { qputenv("PATH", oldPath); });
        const QString real7z = QStandardPaths::findExecutable("7z");
        const QString marker = tool.filePath("indexing");
        const QString release = tool.filePath("release");
        const QString wrapper = tool.filePath("7z");
        QVERIFY(write(wrapper, "#!/bin/bash\n"
                      "if [[ \"$1\" == l && \"$*\" == *z-slow.cbz* ]]; then\n"
                      "touch '" + marker.toUtf8() + "'\n"
                      "while [[ ! -f '" + release.toUtf8() + "' ]]; do sleep 0.02; done\nfi\n"
                      "exec '" + real7z.toUtf8() + "' \"$@\"\n"));
        QVERIFY(QFile::setPermissions(wrapper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("PATH", tool.path().toUtf8() + ':' + oldPath);
        QVERIFY(QFile::copy(library.filePath("one.cbz"), library.filePath("z-slow.cbz")));
        const auto releaseScanner = qScopeGuard([release] { write(release, "done"); });
        backend.scanLibrary();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        QVERIFY(backend.scanning());
        backend.closeReader();
        backend.openComic(library.filePath("one.cbz"));
        QTRY_VERIFY(!backend.pagePath().isEmpty());
        backend.nextPage();
        QTRY_COMPARE(backend.currentPage(), 2);
        QVERIFY(backend.scanning());
        QVERIFY(write(release, "done"));
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        QCOMPARE(backend.comics().size(), 3);
        QVERIFY(backend.reading());
        backend.closeReader();
        Backend reopened(data.path(), cache.path());
        QVERIFY(!reopened.scanning());
        QSignalSpy previews(&reopened, &Backend::previewRequested);
        reopened.applyStartupBehavior();
        QVERIFY(!reopened.reading());
        QCOMPARE(previews.size(), 0);
        QVERIFY(reopened.setPreference("startupBehavior", 1));
        reopened.applyStartupBehavior();
        QCOMPARE(previews.size(), 1);
        QVERIFY(!reopened.reading());
        QVERIFY(reopened.setPreference("startupBehavior", 2));
        reopened.applyStartupBehavior();
        QTRY_VERIFY(!reopened.pagePath().isEmpty());
        QCOMPARE(reopened.currentPage(), 2);
        QVERIFY(reopened.rightToLeft());
        reopened.openComic(library.filePath("z-slow.cbz"));
        QVERIFY(!reopened.rightToLeft());
        QTRY_VERIFY(!reopened.pagePath().isEmpty());
        reopened.closeReader();
    }

    void persistsBackupLocationWithoutMovingOldBackups() {
        QTemporaryDir data, cache, backups;
        QVERIFY(write(data.filePath("library.json"),
                      R"({"folders":[],"books":[{"path":"/one.cbz","title":"One","lastRead":"2026-01-01T00:00:00Z"}]})"));
        Backend backend(data.path(), cache.path());
        const QString original = backend.backupDirectory();
        const QStringList oldBackups = QDir(original).entryList({"*.json"}, QDir::Files);
        QCOMPARE(oldBackups.size(), 1);
        const QString chosen = backups.filePath("chosen");
        QVERIFY(QDir().mkpath(chosen));
        QSignalSpy changed(&backend, &Backend::backupDirectoryChanged);
        QVERIFY(backend.setBackupDirectory(chosen));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(backend.backupDirectory(), chosen);
        backend.backupLibrary();
        const QStringList manual = QDir(chosen).entryList({"omacomics-*.json"}, QDir::Files);
        QCOMPARE(manual.size(), 1);
        backend.removeFromRecent("/one.cbz");
        QCOMPARE(QDir(chosen).entryList({"daily-*.json"}, QDir::Files).size(), 1);
        QCOMPARE(QDir(chosen).entryList({"safety-*.json"}, QDir::Files).size(), 1);
        QCOMPARE(QDir(original).entryList({"*.json"}, QDir::Files), oldBackups);
        backend.restoreLibrary(QDir(chosen).filePath(manual.first()));
        QCOMPARE(backend.backupDirectory(), chosen);
        Backend reopened(data.path(), cache.path());
        QCOMPARE(reopened.backupDirectory(), chosen);
        QVERIFY(QDir(backups.path()).rename("chosen", "offline"));
        reopened.backupLibrary();
        QVERIFY(reopened.status().contains("unavailable"));
        QVERIFY(!QFileInfo::exists(chosen));
        QVERIFY(reopened.setBackupDirectory(""));
        QCOMPARE(reopened.backupDirectory(), original);
        reopened.backupLibrary();
        QCOMPARE(QDir(original).entryList({"*.json"}, QDir::Files).size(), oldBackups.size() + 1);
        QCOMPARE(Backend(data.path(), cache.path()).backupDirectory(), original);
    }

    void rejectsUnusableBackupSettings() {
        QTemporaryDir data, cache, backups;
        Backend backend(data.path(), cache.path());
        const QString original = backend.backupDirectory();
        QVERIFY(!backend.setBackupDirectory("relative/path"));
        QVERIFY(!backend.setBackupDirectory(data.filePath("missing")));
        QVERIFY(write(data.filePath("not-a-folder"), "example"));
        QVERIFY(!backend.setBackupDirectory(data.filePath("not-a-folder")));
        QCOMPARE(backend.backupDirectory(), original);
        QVERIFY(QDir().mkpath(data.filePath("settings.json")));
        QVERIFY(!backend.setBackupDirectory(backups.path()));
        QVERIFY(backend.status().startsWith("Settings could not be saved:"));
        QCOMPARE(backend.backupDirectory(), original);
    }

    void settingsPage_data() { browsesTreeAndReviewsComicVine_data(); }

    void settingsPage() {
        QFETCH(QString, language);
        QFETCH(int, width);
        QFETCH(int, height);
        QTemporaryDir data, cache, backups;
        QVERIFY(write(data.filePath("library.json"),
                      R"({"folders":[],"thumbnailSize":0,"books":[{"path":"/one.cbz","title":"One","format":"CBZ","pageCount":2}]})"));
        QVERIFY(write(data.filePath("language"), language.toUtf8()));
        const QString colors = qEnvironmentVariable("OMACOMICS_TEST_COLORS");
        if (!colors.isEmpty()) {
            QVERIFY(QDir().mkpath(data.filePath("theme")));
            QVERIFY(QFile::copy(colors, data.filePath("theme/colors.toml")));
        }
        Translator translator(data.filePath("language"));
        QCoreApplication::installTranslator(&translator);
        Theme theme(data.path());
        Backend backend(data.path(), cache.path());
        FolderPicker folderPicker;
        FeedbackBrowser browser;
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("folderPicker", &folderPicker);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->resize(width, height);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        const auto click = [window](QQuickItem *control) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              control->mapToScene(QPointF(control->width() / 2, control->height() / 2)).toPoint());
        };
        const auto snapshot = [window](const QString &name) {
            const QString directory = qEnvironmentVariable("OMACOMICS_TEST_SCREENSHOTS");
            if (directory.isEmpty())
                return true;
            QTest::qWait(250);
            return window->grabWindow().save(QDir(directory).filePath(QString::fromLatin1(QTest::currentDataTag())
                                                                    + "-settings-" + name + ".png"));
        };
        auto *search = item(window->contentItem(), "searchField");
        auto *button = item(window->contentItem(), "settingsButton");
        auto *pane = item(window->contentItem(), "settingsPane");
        auto *navigation = item(window->contentItem(), "libraryNavigation");
        QVERIFY(search);
        QVERIFY(button);
        QVERIFY(pane);
        QVERIFY(navigation);
        search->setProperty("text", "One");
        window->setProperty("selectedPath", "/one.cbz");
        backend.selectComic("/one.cbz", true);
        QVERIFY(browser.urls.isEmpty());
        click(button);
        QVERIFY(pane->isVisible());
        QVERIFY(!navigation->isVisible());
        auto *scroll = item(pane, "settingsScroll");
        QVERIFY(scroll);
        auto *flick = scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(flick);
        const auto reveal = [flick](QQuickItem *control) {
            QTest::qWait(30);
            const double top = control->mapToItem(flick, QPointF()).y();
            const double limit = qMax(0., flick->property("contentHeight").toDouble() - flick->height());
            flick->setProperty("contentY", qBound(0., flick->property("contentY").toDouble() + top - 8, limit));
            QTest::qWait(20);
        };
        auto *startup = item(pane, "startupBehavior");
        QVERIFY(startup);
        startup->setProperty("currentIndex", 1);
        QVERIFY(QMetaObject::invokeMethod(startup, "activated", Q_ARG(int, 1)));
        QCOMPARE(backend.preferences().value("startupBehavior").toInt(), 1);
        QVERIFY(snapshot("startup"));
        pane->setProperty("section", 1);
        auto *fit = item(pane, "settingsFitMode");
        QVERIFY(fit);
        fit->setProperty("currentIndex", 2);
        QVERIFY(QMetaObject::invokeMethod(fit, "activated", Q_ARG(int, 2)));
        QCOMPARE(backend.preferences().value("fitMode").toInt(), 2);
        auto *zoom = item(pane, "settingsZoom");
        QVERIFY(zoom);
        reveal(zoom);
        zoom->forceActiveFocus();
        QTest::keyClick(window, language == "ar" ? Qt::Key_Left : Qt::Key_Right);
        QCOMPARE(backend.preferences().value("zoomStep").toInt(), 1);
        QVERIFY(snapshot("reading"));
        auto *doublePage = item(pane, "settingsDoublePage");
        auto *arrowNavigation = item(pane, "settingsArrowNavigation");
        QVERIFY(doublePage);
        QVERIFY(arrowNavigation);
        reveal(doublePage);
        click(doublePage);
        QVERIFY(backend.doublePage());
        reveal(arrowNavigation);
        click(arrowNavigation);
        QVERIFY(!backend.arrowNavigation());
        QVERIFY(snapshot("navigation"));
        pane->setProperty("section", 2);
        auto *startupScan = item(pane, "scanOnStartup");
        auto *view = item(pane, "settingsLibraryView");
        QVERIFY(startupScan);
        QVERIFY(view);
        reveal(startupScan);
        click(startupScan);
        QVERIFY(!backend.preferences().value("scanOnStartup").toBool());
        view->setProperty("currentIndex", 1);
        QVERIFY(QMetaObject::invokeMethod(view, "activated", Q_ARG(int, 1)));
        QVERIFY(backend.treeView());
        QVERIFY(snapshot("library"));
        pane->setProperty("section", 4);
        QTRY_VERIFY(!backend.cacheBusy());
        QVERIFY(snapshot("cache"));
        auto *clearCache = item(pane, "clearCacheButton");
        QVERIFY(clearCache);
        reveal(clearCache);
        click(clearCache);
        auto *clearDialog = window->findChild<QObject *>("clearCacheDialog");
        QVERIFY(clearDialog);
        QTRY_VERIFY(clearDialog->property("opened").toBool());
        QVERIFY(snapshot("clear-cache"));
        QVERIFY(QMetaObject::invokeMethod(clearDialog, "reject"));
        QTRY_VERIFY(!clearDialog->property("visible").toBool());
        pane->setProperty("section", 5);
        auto *seriesLookup = item(pane, "useSeriesForLookup");
        QVERIFY(seriesLookup);
        reveal(seriesLookup);
        auto *indicator = seriesLookup->property("indicator").value<QQuickItem *>();
        auto *caption = seriesLookup->property("contentItem").value<QQuickItem *>();
        QVERIFY(indicator);
        QVERIFY(caption);
        const qreal leftPadding = caption->property("leftPadding").toReal();
        const qreal rightPadding = caption->property("rightPadding").toReal();
        const QRectF captionRect = caption->mapRectToItem(seriesLookup,
            QRectF(leftPadding, 0, caption->width() - leftPadding - rightPadding, caption->height()));
        QVERIFY(!indicator->mapRectToItem(seriesLookup, indicator->boundingRect()).intersects(captionRect));
        click(seriesLookup);
        QVERIFY(!backend.preferences().value("useSeriesForLookup").toBool());
        QVERIFY(snapshot("comic-vine"));
        auto *configure = item(pane, "configureComicVineButton");
        QVERIFY(configure);
        reveal(configure);
        click(configure);
        auto *scrape = window->findChild<QObject *>("scrapeDialog");
        QVERIFY(scrape);
        QTRY_VERIFY(scrape->property("opened").toBool());
        QVERIFY(snapshot("key"));
        QVERIFY(QMetaObject::invokeMethod(scrape, "reject"));
        QTRY_VERIFY(!scrape->property("visible").toBool());
        pane->setProperty("section", 3);
        QVERIFY(snapshot("backups"));
        emit folderPicker.backupFolderSelected(backups.path());
        QCOMPARE(backend.backupDirectory(), backups.path());
        auto *location = item(pane, "backupLocationLabel");
        QVERIFY(location);
        QCOMPARE(location->property("text").toString(), backups.path());
        auto *backup = item(pane, "backupNowButton");
        QVERIFY(backup);
        reveal(backup);
        click(backup);
        QCOMPARE(QDir(backups.path()).entryList({"omacomics-*.json"}, QDir::Files).size(), 1);
        emit folderPicker.restoreFileSelected(backups.filePath("example.json"));
        auto *restore = window->findChild<QObject *>("restoreDialog");
        QVERIFY(restore);
        QTRY_VERIFY(restore->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!restore->property("visible").toBool());
        QVERIFY(pane->isVisible());
        pane->setProperty("section", 6);
        QTest::qWait(50);
        QVERIFY(snapshot("feedback"));
        auto *bug = item(pane, "reportBugButton");
        auto *feature = item(pane, "requestFeatureButton");
        QVERIFY(bug);
        QVERIFY(feature);
        click(bug);
        click(feature);
        QCOMPARE(browser.urls.size(), 2);
        for (int i = 0; i < browser.urls.size(); ++i) {
            const QUrl url = browser.urls.at(i);
            QCOMPARE(url.scheme(), QString("https"));
            QCOMPARE(url.host(), QString("github.com"));
            QCOMPARE(url.path(), QString("/kelmankenberg/OmaComics/issues/new"));
            const QString expected = i == 0 ? "bug_report.yml" : "feature_request.yml";
            QCOMPARE(QUrlQuery(url).queryItemValue("template"), expected);
            const QString file = QFINDTESTDATA(QString("../.github/ISSUE_TEMPLATE/") + expected);
            QVERIFY(QFileInfo::exists(file));
            QCOMPARE(QUrlQuery(url).queryItems().size(), 1);
        }
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!pane->isVisible());
        QVERIFY(navigation->isVisible());
        QCOMPARE(search->property("text").toString(), QString("One"));
        QCOMPARE(window->property("selectedPath").toString(), QString("/one.cbz"));
        QCOMPARE(backend.selectedComics(), QStringList{"/one.cbz"});
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QVERIFY(pane->isVisible());
        click(item(pane, "closeSettingsButton"));
        QVERIFY(!pane->isVisible());
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QVERIFY(!window->property("libraryVisible").toBool());
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QVERIFY(window->property("libraryVisible").toBool());
        QVERIFY(pane->isVisible());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!pane->isVisible());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
        QLocale::setDefault(QLocale("en"));
    }

    void backsUpRestoresAndRemovesRecentWithoutLosingProgress() {
        QTemporaryDir data, cache, backups;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(backups.isValid());
        QVERIFY(write(data.filePath("library.json"),
                      R"({"folders":[],"books":[)"
                      R"({"path":"/a.cbz","title":"Alpha","collection":"Favorites","progress":7,"bookmarks":[3],"lastRead":"2026-01-01T00:00:00Z"},)"
                      R"({"path":"/b.cbz","title":"Beta"}]})"));
        Backend backend(data.path(), cache.path());
        backend.backupLibrary(backups.path());
        const QStringList manual = QDir(backups.path()).entryList({"*.json"}, QDir::Files);
        QCOMPARE(manual.size(), 1);
        backend.setCollectionFilter("__recent__");
        QCOMPARE(backend.visibleComics().size(), 1);
        backend.removeFromRecent("/a.cbz");
        QCOMPARE(backend.visibleComics().size(), 0);
        backend.setCollectionFilter("");
        QCOMPARE(backend.visibleComics().size(), 2);
        QCOMPARE(backend.comics().first().toMap().value("progress").toInt(), 7);
        QCOMPARE(backend.comics().first().toMap().value("bookmarks").toList(), QVariantList{3});
        QCOMPARE(QDir(data.filePath("backups")).entryList({"daily-*.json"}, QDir::Files).size(), 1);
        QCOMPARE(QDir(data.filePath("backups")).entryList({"safety-*.json"}, QDir::Files).size(), 1);
        QVERIFY(write(backups.filePath("invalid.json"), R"({"books":[{}],"folders":[]})"));
        backend.restoreLibrary(backups.filePath("invalid.json"));
        QCOMPARE(backend.comics().size(), 2);
        QVERIFY(backend.status().contains("invalid"));
        backend.restoreLibrary(backups.filePath(manual.first()));
        QCOMPARE(backend.comics().size(), 2);
        backend.setCollectionFilter("__recent__");
        QCOMPARE(backend.visibleComics().size(), 1);
        QCOMPARE(backend.visibleComics().first().toMap().value("progress").toInt(), 7);
        QCOMPARE(QDir(data.filePath("backups")).entryList({"safety-*.json"}, QDir::Files).size(), 2);
    }

    void backsUpBeforeRemovingFolder() {
        QTemporaryDir data, cache, library;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(library.isValid());
        const QString folder = QFileInfo(library.path()).canonicalFilePath();
        const QString path = folder + "/sample.cbz";
        const QJsonObject state{{"folders", QJsonArray{folder}}, {"current", path},
                                {"books", QJsonArray{QJsonObject{{"path", path}, {"title", "Sample"},
                                                                 {"progress", 4}, {"pageCount", 8}}}}};
        QVERIFY(write(data.filePath("library.json"), QJsonDocument(state).toJson()));
        Backend backend(data.path(), cache.path());
        QTRY_VERIFY(!backend.scanning());
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), true);
        QVERIFY(QDir(data.path()).rename("backups", "backups-saved"));
        QVERIFY(write(data.filePath("backups"), "not a directory"));
        backend.removeLibraryFolder(folder);
        QCOMPARE(backend.comics().size(), 1);
        QCOMPARE(backend.libraryFolders().size(), 1);
        QVERIFY(QFile::remove(data.filePath("backups")));
        QVERIFY(QDir(data.path()).rename("backups-saved", "backups"));
        backend.removeLibraryFolder(folder);
        QCOMPARE(backend.comics().size(), 0);
        QCOMPARE(backend.libraryFolders().size(), 0);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), false);
        const QStringList saved = QDir(data.filePath("backups")).entryList({"safety-*.json"}, QDir::Files);
        QCOMPARE(saved.size(), 1);
        backend.restoreLibrary(data.filePath("backups/" + saved.first()));
        QCOMPARE(backend.comics().size(), 1);
        QCOMPARE(backend.comics().first().toMap().value("progress").toInt(), 4);
        QCOMPARE(backend.libraryFolders().size(), 1);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), true);
        QTRY_VERIFY(!backend.scanning());
    }

    void scansComicInfoAndReadsPages() {
        QTemporaryDir data, cache, library;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(library.isValid());
        QVERIFY(write(library.filePath("ComicInfo.xml"),
                      "<ComicInfo><Title>Sample Issue</Title><Series>Test Series</Series>"
                      "<Number>7</Number><Writer>Reader</Writer></ComicInfo>"));
        QImage page(4, 4, QImage::Format_RGB32);
        page.fill(Qt::red);
        QVERIFY(page.save(library.filePath("1.png")));
        page.fill(Qt::blue);
        QVERIFY(page.save(library.filePath("2.png")));
        page.fill(Qt::green);
        QVERIFY(page.save(library.filePath("3.png")));
        page.fill(Qt::yellow);
        QVERIFY(page.save(library.filePath("4.png")));

        QProcess archive;
        archive.setWorkingDirectory(library.path());
        archive.start("7z", {"a", "-tzip", library.filePath("sample.cbz"),
                             "ComicInfo.xml", "1.png", "2.png", "3.png", "4.png"});
        QVERIFY(archive.waitForStarted());
        QVERIFY(archive.waitForFinished(10000));
        QCOMPARE(archive.exitCode(), 0);

        Backend backend(data.path(), cache.path());
        backend.addLibraryFolder(library.path());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        QCOMPARE(backend.comics().size(), 1);
        const QVariantMap comic = backend.comics().first().toMap();
        QCOMPARE(comic.value("title").toString(), QString("Sample Issue"));
        QCOMPARE(comic.value("series").toString(), QString("Test Series"));
        QCOMPARE(comic.value("pageCount").toInt(), 4);

        const QString path = comic.value("path").toString();
        backend.ensureCover(path);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.comics().first().toMap().value("cover").toString().isEmpty(), 5000);
        backend.openComic(path);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 5000);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), true);
        QVERIFY(QFileInfo::exists(QUrl(backend.pagePath()).toLocalFile()));
        QCOMPARE(backend.currentPage(), 1);
        backend.setDoublePage(true);
        QVERIFY(backend.secondPagePath().isEmpty());
        backend.nextPage();
        QCOMPARE(backend.currentPage(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.secondPagePath().isEmpty(), 5000);
        backend.nextPage();
        QCOMPARE(backend.currentPage(), 4);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), false);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.secondPagePath().isEmpty(), 5000);
        backend.toggleBookmark();
        QVERIFY(backend.currentBookmarked());
        QCOMPARE(backend.bookmarks().size(), 1);
        QCOMPARE(backend.bookmarks().first().toInt(), 4);
        backend.openPage(3);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), false);
        backend.setDoublePage(false);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), true);
        backend.setDoublePage(true);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), false);
        backend.openPage(1);
        QCOMPARE(backend.currentPage(), 1);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), true);
        backend.setCollectionFilter("__recent__");
        QCOMPARE(backend.visibleComics().size(), 1);
        backend.previousPage();
        QCOMPARE(backend.currentPage(), 1);
    }

    void restoresReadingStatusOnStartup() {
        QTemporaryDir data, cache, emptyData, emptyCache;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(emptyData.isValid());
        QVERIFY(emptyCache.isValid());
        Backend empty(emptyData.path(), emptyCache.path());
        QCOMPARE(readingStatus(emptyData.path()).value("inProgress").toBool(), false);
        QVERIFY(write(data.filePath("library.json"),
                      R"({"folders":[],"current":"/comic.cbz","doublePage":true,"books":[)"
                      R"({"path":"/comic.cbz","title":"Sample","pageCount":5,"progress":4}]})"));
        Backend backend(data.path(), cache.path());
        QJsonObject status = readingStatus(data.path());
        QCOMPARE(status.value("page").toInt(), 4);
        QCOMPARE(status.value("pageCount").toInt(), 5);
        QCOMPARE(status.value("inProgress").toBool(), false);
        backend.setDoublePage(false);
        QCOMPARE(readingStatus(data.path()).value("inProgress").toBool(), true);
        Backend reopened(data.path(), cache.path());
        status = readingStatus(data.path());
        QCOMPARE(status.value("page").toInt(), 4);
        QCOMPARE(status.value("pageCount").toInt(), 5);
        QCOMPARE(status.value("inProgress").toBool(), true);
        QVERIFY(write(emptyData.filePath("library.json"),
                      R"({"folders":[],"current":"/single.cbz","books":[)"
                      R"({"path":"/single.cbz","title":"One page","pageCount":1,"progress":1}]})"));
        Backend single(emptyData.path(), emptyCache.path());
        QCOMPARE(readingStatus(emptyData.path()).value("inProgress").toBool(), false);
    }

    void scansAndReadsPdf() {
        QTemporaryDir data, cache, library;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(library.isValid());
        QPdfWriter pdf(library.filePath("notes.pdf"));
        pdf.setPageSize(QPageSize(QPageSize::A4));
        QPainter painter(&pdf);
        QVERIFY(painter.isActive());
        painter.drawText(100, 100, "Sample comic page");
        painter.end();

        Backend backend(data.path(), cache.path());
        backend.addLibraryFolder(library.path());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        QCOMPARE(backend.comics().size(), 1);
        QCOMPARE(backend.comics().first().toMap().value("pageCount").toInt(), 1);
        backend.openComic(backend.comics().first().toMap().value("path").toString());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 10000);
        QVERIFY(QFileInfo::exists(QUrl(backend.pagePath()).toLocalFile()));
    }

    void indexesOnlyNewAndChangedComics() {
        QTemporaryDir data, cache, library;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(library.isValid());
        QImage page(8, 8, QImage::Format_RGB32);
        page.fill(Qt::red);
        QVERIFY(page.save(library.filePath("1.png")));
        QVERIFY(write(library.filePath("ComicInfo.xml"), "<ComicInfo><Writer>Sample Writer</Writer></ComicInfo>"));
        const auto archive = [&](const QString &name, const QStringList &entries) {
            QProcess process;
            process.setWorkingDirectory(library.path());
            QStringList args{"a", "-tzip", library.filePath(name)};
            args.append(entries);
            process.start("7z", args);
            return process.waitForStarted() && process.waitForFinished(10000) && process.exitCode() == 0;
        };
        QVERIFY(archive("first.cbz", {"1.png", "ComicInfo.xml"}));
        QVERIFY(archive("second.cbz", {"1.png"}));
        const QString first = library.filePath("first.cbz");
        const QString second = library.filePath("second.cbz");
        {
            Backend backend(data.path(), cache.path());
            backend.addLibraryFolder(library.path());
            QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
            QCOMPARE(backend.comics().size(), 2);
            backend.openComic(first);
            QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 5000);
        }
        {
            Backend backend(data.path(), cache.path());
            QStringList statuses;
            connect(&backend, &Backend::statusChanged, this, [&] { statuses.append(backend.status()); });
            QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
            QVERIFY(std::none_of(statuses.cbegin(), statuses.cend(), [](const QString &status) {
                return status.startsWith("Indexing ");
            }));
            backend.resumeComic();
            QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 5000);
            QVERIFY(!backend.scanning());
        }
        page.fill(Qt::blue);
        QVERIFY(page.save(library.filePath("2.png")));
        QVERIFY(archive("second.cbz", {"2.png"}));
        QVERIFY(archive("third.cbz", {"1.png"}));
        {
            Backend backend(data.path(), cache.path());
            QStringList statuses;
            connect(&backend, &Backend::statusChanged, this, [&] { statuses.append(backend.status()); });
            QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
            QVERIFY(statuses.contains("Indexing second.cbz"));
            QVERIFY(statuses.contains("Indexing third.cbz"));
            QVERIFY(!statuses.contains("Indexing first.cbz"));
            QCOMPARE(backend.comics().size(), 3);
            backend.openComic(second);
            QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 5000);
            QCOMPARE(backend.pageCount(), 2);
            backend.nextPage();
            QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 5000);
            QCOMPARE(QImage(QUrl(backend.pagePath()).toLocalFile()).pixelColor(0, 0), QColor(Qt::blue));
        }
        page.fill(Qt::green);
        QVERIFY(page.save(library.filePath("1.png")));
        QVERIFY(QFile::remove(first));
        QVERIFY(archive("first.cbz", {"1.png", "2.png"}));
        {
            Backend backend(data.path(), cache.path());
            connect(&backend, &Backend::statusChanged, this, [&] {
                if (backend.status() == "Indexing first.cbz")
                    backend.openComic(first);
            });
            QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 10000);
            QCOMPARE(QImage(QUrl(backend.pagePath()).toLocalFile()).pixelColor(0, 0), QColor(Qt::green));
            QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
            const auto updated = backend.comics();
            const auto book = std::find_if(updated.cbegin(), updated.cend(), [&first](const QVariant &value) {
                return value.toMap().value("path").toString() == first;
            });
            QVERIFY(book != updated.cend());
            QVERIFY(book->toMap().value("writer").toString().isEmpty());
        }
    }

    void migratesExistingLibraryWithoutReindexing() {
        QTemporaryDir data, cache, newCache, library;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(newCache.isValid());
        QVERIFY(library.isValid());
        QImage page(8, 8, QImage::Format_RGB32);
        page.fill(Qt::red);
        QVERIFY(page.save(library.filePath("1.png")));
        for (const QString &name : {QStringLiteral("first.cbz"), QStringLiteral("second.cbz")}) {
            QProcess archive;
            archive.setWorkingDirectory(library.path());
            archive.start("7z", {"a", "-tzip", library.filePath(name), "1.png"});
            QVERIFY(archive.waitForStarted());
            QVERIFY(archive.waitForFinished(10000));
            QCOMPARE(archive.exitCode(), 0);
        }
        {
            Backend backend(data.path(), cache.path());
            backend.addLibraryFolder(library.path());
            QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
            QCOMPARE(backend.comics().size(), 2);
        }
        QFile file(data.filePath("library.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonArray legacyBooks = state.value("books").toArray();
        for (int i = 0; i < legacyBooks.size(); ++i) {
            QJsonObject book = legacyBooks.at(i).toObject();
            book.remove("indexStamp");
            legacyBooks.replace(i, book);
        }
        state.insert("books", legacyBooks);
        QVERIFY(write(data.filePath("library.json"), QJsonDocument(state).toJson()));
        Backend backend(data.path(), newCache.path());
        QStringList statuses;
        connect(&backend, &Backend::statusChanged, this, [&] { statuses.append(backend.status()); });
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        QVERIFY(std::none_of(statuses.cbegin(), statuses.cend(), [](const QString &status) {
            return status.startsWith("Indexing ");
        }));
        QCOMPARE(backend.comics().size(), 2);
        backend.openComic(library.filePath("first.cbz"));
        QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 10000);
        QVERIFY(statuses.contains("Indexing first.cbz"));
        QVERIFY(!statuses.contains("Indexing second.cbz"));
        backend.ensureCover(library.filePath("second.cbz"));
        QTRY_VERIFY_WITH_TIMEOUT(statuses.contains("Indexing second.cbz"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        const auto books = backend.comics();
        const auto second = std::find_if(books.cbegin(), books.cend(), [&](const QVariant &value) {
            return value.toMap().value("path").toString() == library.filePath("second.cbz");
        });
        QVERIFY(second != books.cend());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.comics().last().toMap().value("cover").toString().isEmpty(), 10000);
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
        QCOMPARE(theme.radius(), 0);
    }

    void togglesLibraryWhileReading() {
        QTemporaryDir data, cache, library;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(library.isValid());
        QImage page(8, 8, QImage::Format_RGB32);
        page.fill(Qt::red);
        QVERIFY(page.save(library.filePath("1.png")));
        QVERIFY(page.save(library.filePath("2.png")));
        QVERIFY(page.save(library.filePath("3.png")));
        QProcess archive;
        archive.setWorkingDirectory(library.path());
        archive.start("7z", {"a", "-tzip", library.filePath("sample.cbz"), "1.png", "2.png", "3.png"});
        QVERIFY(archive.waitForStarted());
        QVERIFY(archive.waitForFinished(10000));
        QCOMPARE(archive.exitCode(), 0);

        Translator translator(data.filePath("language"));
        QCoreApplication::installTranslator(&translator);
        Theme theme(data.path());
        Backend backend(data.path(), cache.path());
        FolderPicker folderPicker;
        backend.addLibraryFolder(library.path());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        backend.openComic(library.filePath("sample.cbz"));
        QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 5000);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("folderPicker", &folderPicker);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        auto *panel = item(window->contentItem(), "libraryPanel");
        QVERIFY(panel);
        QVERIFY(panel->isVisible());
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(!panel->isVisible());
        QVERIFY(backend.reading());
        QCOMPARE(backend.currentPage(), 1);
        QVERIFY(!backend.currentBookmarked());
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(!backend.currentBookmarked());
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        auto *settings = item(window->contentItem(), "settingsPane");
        QVERIFY(settings);
        QVERIFY(settings->isVisible());
        QVERIFY(backend.reading());
        auto *restoreButton = item(settings, "restoreBackupButton");
        QVERIFY(restoreButton);
        QVERIFY(!restoreButton->isEnabled());
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(backend.currentPage(), 1);
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!settings->isVisible());
        QVERIFY(backend.reading());
        QCOMPARE(backend.currentPage(), 1);
        auto *viewport = item(window->contentItem(), "pageViewport");
        QVERIFY(viewport);
        viewport->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(backend.currentPage(), 2);
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(backend.currentPage(), 1);
        backend.setRightToLeft(true);
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(backend.currentPage(), 2);
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(backend.currentPage(), 1);
        backend.setRightToLeft(false);
        backend.setArrowNavigation(false);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(backend.currentPage(), 1);
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(backend.currentPage(), 2);
        QTest::keyClick(window, Qt::Key_Backspace);
        QCOMPARE(backend.currentPage(), 1);
        backend.setArrowNavigation(true);
        auto *search = item(window->contentItem(), "searchField");
        QVERIFY(search);
        search->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(backend.currentPage(), 1);
        viewport->forceActiveFocus();
        const auto center = [viewport] {
            return viewport->mapToScene(QPointF(viewport->width() / 2, viewport->height() / 2)).toPoint();
        };
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center());
        QTRY_COMPARE(backend.currentPage(), 2);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center());
        QTRY_COMPARE(backend.currentPage(), 1);
        backend.setRightToLeft(true);
        backend.setDoublePage(true);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center());
        QTRY_COMPARE(backend.currentPage(), 2);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center());
        QCOMPARE(backend.currentPage(), 2);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center());
        QTRY_COMPARE(backend.currentPage(), 1);
        backend.setDoublePage(false);
        backend.setRightToLeft(false);
        backend.setMouseNavigation(false);
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, center());
        QCOMPARE(backend.currentPage(), 1);
        backend.setMouseNavigation(true);
        const auto wheel = [window, &center](int angle) {
            const QPoint position = center();
            QWheelEvent event(position, window->mapToGlobal(position), QPoint(), QPoint(0, angle),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &event);
        };
        wheel(-120);
        QTRY_COMPARE(backend.currentPage(), 2);
        wheel(120);
        QTRY_COMPARE(backend.currentPage(), 1);
        backend.setWheelNavigation(false);
        wheel(-120);
        QCOMPARE(backend.currentPage(), 1);
        backend.setWheelNavigation(true);
        QVERIFY(backend.setPreference("zoomStep", 4));
        QTRY_VERIFY(viewport->property("contentHeight").toReal() > viewport->height() + 100);
        viewport->setProperty("contentY", 0);
        wheel(-120);
        QCOMPARE(backend.currentPage(), 1);
        QTRY_VERIFY(viewport->property("contentY").toReal() > 0);
        viewport->setProperty("contentY",
                              viewport->property("contentHeight").toReal() - viewport->height());
        wheel(-120);
        QTRY_COMPARE(backend.currentPage(), 2);
        QTRY_VERIFY(viewport->property("contentY").toReal() < 1);
        wheel(120);
        QTRY_COMPARE(backend.currentPage(), 1);
        QTRY_VERIFY(viewport->property("contentY").toReal()
                    >= viewport->property("contentHeight").toReal() - viewport->height() - 1);
        const QPoint start = center();
        const qreal beforeDrag = viewport->property("contentY").toReal();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(0, 30), 100);
        QTest::mouseMove(window, start + QPoint(0, 100), 100);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(0, 100));
        QCOMPARE(backend.currentPage(), 1);
        QTRY_VERIFY(viewport->property("contentY").toReal() < beforeDrag);
        auto *pageScrollbar = item(window->contentItem(), "readerScrollBar");
        QVERIFY(pageScrollbar);
        viewport->setProperty("contentY", 0);
        const QPoint scrollStart = pageScrollbar->mapToScene(
            QPointF(pageScrollbar->width() / 2,
                    pageScrollbar->height() * pageScrollbar->property("visualSize").toReal() / 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, scrollStart);
        QTest::mouseMove(window, scrollStart + QPoint(0, 30), 100);
        QTest::mouseMove(window, scrollStart + QPoint(0, 100), 100);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, scrollStart + QPoint(0, 100));
        QCOMPARE(backend.currentPage(), 1);
        QTRY_VERIFY(viewport->property("contentY").toReal() > 0);
        backend.setWheelNavigation(false);
        viewport->setProperty("contentY", 0);
        wheel(-120);
        QTRY_VERIFY(viewport->property("contentY").toReal() > 0);
        QCOMPARE(backend.currentPage(), 1);
        backend.setWheelNavigation(true);
        auto *menuButton = item(window->contentItem(), "navigationMenuButton");
        auto *navigationMenu = window->findChild<QObject *>("navigationMenu");
        QVERIFY(menuButton);
        QVERIFY(navigationMenu);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          menuButton->mapToScene(QPointF(menuButton->width() / 2, menuButton->height() / 2)).toPoint());
        QTRY_VERIFY(navigationMenu->property("opened").toBool());
        auto *wheelOption = item(window->contentItem(), "wheelNavigationOption");
        QTRY_VERIFY(wheelOption && wheelOption->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          wheelOption->mapToScene(QPointF(wheelOption->width() / 2, wheelOption->height() / 2)).toPoint());
        QTRY_VERIFY(!backend.wheelNavigation());
        QTRY_VERIFY(!navigationMenu->property("visible").toBool());
        window->resize(820, 560);
        auto *closeReader = item(window->contentItem(), "closeReaderButton");
        auto *controls = item(window->contentItem(), "readerControls");
        QVERIFY(closeReader);
        QVERIFY(controls);
        QTRY_VERIFY(closeReader->mapToScene(QPointF(closeReader->width(), closeReader->height())).x()
                    <= window->width());
        QTRY_VERIFY(controls->mapToScene(QPointF(0, controls->height())).y() <= window->height());
        QVERIFY(controls->height() > 60);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }

    void previewsSelectedComicWithoutReading() {
        QTemporaryDir data, cache, library;
        QVERIFY(data.isValid());
        QVERIFY(cache.isValid());
        QVERIFY(library.isValid());
        const QString first = library.filePath("first.cbz");
        const QString second = library.filePath("second.cbz");
        for (const auto &sample : {qMakePair(QStringLiteral("first"), QColor(Qt::red)),
                                   qMakePair(QStringLiteral("second"), QColor(Qt::blue))}) {
            QImage page(400, 600, QImage::Format_RGB32);
            page.fill(sample.second);
            QPainter painter(&page);
            painter.setPen(QPen(Qt::white, 8));
            painter.drawRect(20, 20, 360, 560);
            painter.setPen(Qt::white);
            painter.drawText(QRect(35, 40, 330, 100), Qt::AlignCenter, "Sample cover");
            painter.end();
            const QString image = sample.first + ".png";
            QVERIFY(page.save(library.filePath(image)));
            QVERIFY(write(library.filePath("ComicInfo.xml"),
                          "<ComicInfo><Title>The Sample Voyage</Title><Series>Example Series</Series>"
                          "<Writer>Example Author</Writer><Summary>A fictional comic for previewing the library."
                          "</Summary></ComicInfo>"));
            QProcess archive;
            archive.setWorkingDirectory(library.path());
            archive.start("7z", {"a", "-tzip", library.filePath(sample.first + ".cbz"), image, "ComicInfo.xml"});
            QVERIFY(archive.waitForStarted());
            QVERIFY(archive.waitForFinished(10000));
            QCOMPARE(archive.exitCode(), 0);
        }

        Translator translator(data.filePath("language"));
        QCoreApplication::installTranslator(&translator);
        Theme theme(data.path());
        Backend backend(data.path(), cache.path());
        FolderPicker folderPicker;
        backend.setThumbnailSize(0);
        backend.addLibraryFolder(library.path());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("folderPicker", &folderPicker);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        auto *cover = item(window->contentItem(), "selectedCover");
        QVERIFY(cover);
        auto *firstRow = comicRow(window->contentItem(), first);
        QTRY_VERIFY((firstRow = comicRow(window->contentItem(), first)));
        const auto center = [](QQuickItem *control) {
            return control->mapToScene(QPointF(control->width() / 2, control->height() / 2)).toPoint();
        };
        const auto coverColor = [cover] {
            const QString path = cover->property("source").toUrl().toLocalFile();
            if (path.isEmpty())
                return QColor();
            const QImage image(path);
            return image.isNull() ? QColor() : image.pixelColor(0, 0);
        };
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(firstRow));
        QTRY_COMPARE(window->property("selectedPath").toString(), first);
        QTRY_VERIFY_WITH_TIMEOUT(!cover->property("source").toUrl().isEmpty(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(coverColor(), QColor(Qt::red), 10000);
        QVERIFY(!backend.reading());
        QVERIFY(backend.currentPath().isEmpty());

        auto *secondRow = comicRow(window->contentItem(), second);
        QTRY_VERIFY((secondRow = comicRow(window->contentItem(), second)));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(secondRow));
        QTRY_COMPARE(window->property("selectedPath").toString(), second);
        QTRY_COMPARE_WITH_TIMEOUT(coverColor(), QColor(Qt::blue), 10000);
        QVERIFY(!backend.reading());
        QVERIFY(backend.currentPath().isEmpty());
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, center(secondRow));
        QTRY_VERIFY(backend.reading());
        QCOMPARE(backend.currentPath(), second);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.pagePath().isEmpty(), 5000);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        QVERIFY(write(dir.path() + "/theme/colors.toml", "mode = \"light\"\n"
                                                         "theme_type = \"dark\"\n"
                                                         "accent = \"#112233\"\n"
                                                         "bg = #fafafa # paper\n"
                                                         "foreground = '#202020'\n"
                                                         "color1 = \"#aa0000\"\n"));

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#fafafa"));
        QCOMPARE(theme.foreground(), QColor("#202020"));
        QCOMPARE(theme.colors().value("red").value<QColor>(), QColor("#aa0000"));
        QVERIFY(theme.light());
    }

    void themeAsksHyprlandAgainOnlyWhenItReloads() {
        // The fake Hyprland lives outside the theme directory, whose changes
        // the Theme also follows.
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));

        // Once on start, and once more when the event socket connects.
        Theme theme(dir.path(), nullptr, command, events.fullServerName());
        QTRY_VERIFY(events.hasPendingConnections());
        QLocalSocket *hyprland = events.nextPendingConnection();
        QTRY_COMPARE(lines(hypr.filePath("calls")), 2);
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":10}"));
        QTest::qWait(300);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        // Events arrive in pieces; only a whole configreloaded line counts.
        hyprland->write("activewindow>>kitty,~\nconfigrel");
        hyprland->flush();
        QTest::qWait(100);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        hyprland->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(lines(hypr.filePath("calls")), 3);
    }

    void themeReachesHyprlandOnceItListens() {
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        Theme theme(dir.path(), nullptr, command, hypr.filePath("events.sock"));
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":8}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));
        QTRY_VERIFY_WITH_TIMEOUT(events.hasPendingConnections(), 5000);
        QTRY_COMPARE(theme.radius(), 8);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorSeesAFirstPick() {
        // Before the first pick there is no ~/.config/omarchy to watch.
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/da.json", R"({"Save": "Gem"})"));
        Translator translator(dir.path() + "/omarchy/language", dir.path());
        QVERIFY(QDir(dir.path()).mkpath("omarchy"));
        QVERIFY(write(dir.path() + "/omarchy/language", "da\n"));
        QTRY_COMPARE(translator.translate("Main", "Save"), QString("Gem"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"},
                                                   "%n files": {"one": "%n file", "other": "%n files"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        // An untranslated counted string falls back to English, with English rules.
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QString("%n files"));
    }

    void englishCatalogListsEveryString() {
        // en.json is what translators work from: every text the sources
        // translate is in it, as itself or, when counted, as its English
        // plural forms, and nothing else is.
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QStringList problems;
        const QMap<QString, bool> used = translatables(sources, &problems);
        const QJsonObject english = readCatalog(sources + "/i18n/en.json");
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void parsesWidgetStatusWithoutInterpretingTitles() {
        QFile source(QFINDTESTDATA("../ReadingStatus.js"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QJSEngine engine;
        const QJSValue loaded = engine.evaluate(QString::fromUtf8(source.readAll()));
        QVERIFY2(!loaded.isError(), qPrintable(loaded.toString()));
        const auto parse = [&](const QString &raw, bool readFailed = false) {
            return engine.globalObject().property("parse").call({QJSValue(raw), QJSValue(readFailed)}).toVariant().toMap();
        };
        const QString markup = "<img src=\"https://example.invalid/tracker\"> & <b>Comic</b>";
        auto status = parse(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"title", markup}, {"page", 2}, {"pageCount", 10}, {"progress", 20}}).toJson()));
        QCOMPARE(status.value("title").toString(), markup);
        QVERIFY(status.value("available").toBool());
        QVERIFY(status.value("inProgress").toBool());
        QCOMPARE(status.value("page").toInt(), 2);
        status = parse(R"({"title":"Comic","page":10,"pageCount":10,"progress":100})");
        QVERIFY(!status.value("inProgress").toBool());
        status = parse(R"({"title":"Comic","page":2,"pageCount":10,"inProgress":false})");
        QVERIFY(!status.value("inProgress").toBool());
        status = parse(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"title", QString(300, 'x')}, {"page", 100001}, {"pageCount", -1}, {"progress", 101}}).toJson()));
        QCOMPARE(status.value("title").toString().size(), 256);
        QCOMPARE(status.value("page").toInt(), 100000);
        QCOMPARE(status.value("pageCount").toInt(), 0);
        QCOMPARE(status.value("progress").toInt(), 0);
        for (const QString &invalid : {QString("{"), QString("null"), QString("[]"), QString(8193, 'x')}) {
            status = parse(invalid);
            QVERIFY(!status.value("available").toBool());
            QVERIFY(status.value("title").toString().isEmpty());
            QVERIFY(!status.value("inProgress").toBool());
        }
        status = parse("");
        QVERIFY(status.value("available").toBool());
        QVERIFY(!status.value("inProgress").toBool());
        status = parse("", true);
        QVERIFY(!status.value("available").toBool());
        QVERIFY(!status.value("inProgress").toBool());
    }

    void catalogsMatchTheEnglishOne() {
        // A catalog may lack a string (it shows in English meanwhile), but what
        // it has translates something in en.json, with the same placeholders,
        // and a counted string has every form its language uses.
        const QString folder = QFileInfo(QFINDTESTDATA("../src/i18n/en.json")).absolutePath();
        const QJsonObject english = readCatalog(folder + "/en.json");
        QStringList problems;
        for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
            const QString code = file.completeBaseName();
            const QJsonObject catalog = readCatalog(file.filePath());
            if (catalog.isEmpty())
                problems << code + ": not a JSON object, or empty";
            // Some languages reach a form only at a million ("many" in French).
            QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
            for (long long n = 0; n <= 1000; ++n)
                categories << Translator::pluralCategory(code, n);
            for (auto it = catalog.begin(); it != catalog.end(); ++it) {
                const QString where = code + ": " + it.key() + ": ";
                const QJsonValue source = english.value(it.key());
                if (source.isUndefined()) {
                    problems << where + "not in en.json";
                } else if (source.isString()) {
                    if (!it.value().isString() || it.value().toString().isEmpty())
                        problems << where + "needs a translation";
                    else if (placeholders(it.value().toString()) != placeholders(it.key()))
                        problems << where + "has other placeholders than the English";
                } else {
                    const QJsonObject forms = it.value().toObject();
                    const QStringList wanted = placeholders(source.toObject().value("other").toString());
                    for (const QString &category : categories)
                        if (forms.value(category).toString().isEmpty())
                            problems << where + "needs its " + category + " form";
                    // A form other than "other" may say its number in words.
                    for (auto form = forms.begin(); form != forms.end(); ++form) {
                        const QStringList has = placeholders(form.value().toString());
                        QStringList needed = wanted;
                        if (form.key() != "other")
                            needed.removeIf([&](const QString &p) { return p.endsWith('n') && !has.contains(p); });
                        if (has != needed)
                            problems << where + form.key() + " has other placeholders than the English";
                    }
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void windowLoadsAndAnswersTheKeyboard() {
        // A temp language and theme, so the test never depends on the desktop's.
        QTemporaryDir dir, cache;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        QVERIFY(QDir().mkpath(dir.path() + "/data"));
        QJsonArray books;
        for (int i = 0; i < 20; ++i) {
            QJsonObject book{{"path", QString("/comics/book%1.cbz").arg(i)},
                             {"title", QString("Book %1").arg(i)}, {"format", "CBZ"}, {"pageCount", 1}};
            if (i == 0)
                book.insert("lastRead", "2026-01-01T00:00:00Z");
            if (i == 1)
                book.insert("collection", "Favorites");
            books.append(book);
        }
        const QJsonObject library{{"folders", QJsonArray{"/one", "/two", "/three", "/four", "/five"}},
                                  {"books", books}};
        QVERIFY(write(dir.path() + "/data/library.json", QJsonDocument(library).toJson()));
        Backend backend(dir.path() + "/data", cache.path());
        FolderPicker folderPicker;

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("folderPicker", &folderPicker);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        auto *search = item(window->contentItem(), "searchField");
        QVERIFY(search);
        search->setProperty("text", "sample");
        QCOMPARE(backend.visibleComics().size(), 0);
        search->setProperty("text", "");
        QVERIFY(item(window->contentItem(), "addFolderButton"));

        QTRY_VERIFY(!backend.scanning());
        auto *removeFolder = item(window->contentItem(), "removeLibraryFolderButton");
        QVERIFY(removeFolder);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          removeFolder->mapToScene(QPointF(removeFolder->width() / 2, removeFolder->height() / 2)).toPoint());
        auto *removeDialog = window->findChild<QObject *>("removeFolderDialog");
        QVERIFY(removeDialog);
        QTRY_VERIFY(removeDialog->property("opened").toBool());
        QCOMPARE(backend.libraryFolders().size(), 5);
        QVERIFY(QMetaObject::invokeMethod(removeDialog, "reject"));
        QTRY_VERIFY(!removeDialog->property("visible").toBool());
        QCOMPARE(backend.libraryFolders().size(), 5);
        auto *settingsButton = item(window->contentItem(), "settingsButton");
        auto *settingsPane = item(window->contentItem(), "settingsPane");
        QVERIFY(settingsButton);
        QVERIFY(settingsPane);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          settingsButton->mapToScene(QPointF(settingsButton->width() / 2, settingsButton->height() / 2)).toPoint());
        QVERIFY(settingsPane->isVisible());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!settingsPane->isVisible());
        emit folderPicker.restoreFileSelected(cache.filePath("backup.json"));
        auto *restoreDialog = window->findChild<QObject *>("restoreDialog");
        QVERIFY(restoreDialog);
        QTRY_VERIFY(restoreDialog->property("opened").toBool());
        QCOMPARE(restoreDialog->property("backupPath").toString(), cache.filePath("backup.json"));
        QVERIFY(QMetaObject::invokeMethod(restoreDialog, "reject"));
        QTRY_VERIFY(!restoreDialog->property("visible").toBool());
        QCOMPARE(backend.libraryFolders().size(), 5);
        auto *allBooks = item(window->contentItem(), "allBooksButton");
        auto *recentBooks = item(window->contentItem(), "recentBooksButton");
        auto *filter = item(window->contentItem(), "collectionFilter");
        QVERIFY(allBooks);
        QVERIFY(recentBooks);
        QVERIFY(filter);
        QVERIFY(allBooks->property("highlighted").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          recentBooks->mapToScene(QPointF(recentBooks->width() / 2, recentBooks->height() / 2)).toPoint());
        QTRY_VERIFY(recentBooks->property("highlighted").toBool());
        QVERIFY(!allBooks->property("highlighted").toBool());
        QCOMPARE(backend.visibleComicModel()->rowCount(), 1);
        auto *removeRecent = item(window->contentItem(), "removeRecentBook");
        QTRY_VERIFY(removeRecent && removeRecent->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          removeRecent->mapToScene(QPointF(removeRecent->width() / 2, removeRecent->height() / 2)).toPoint());
        QTRY_COMPARE(backend.visibleComicModel()->rowCount(), 0);
        allBooks->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(backend.visibleComicModel()->rowCount(), 20);
        QVERIFY(allBooks->property("highlighted").toBool());
        filter->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(filter->property("currentIndex").toInt(), 1);
        QCOMPARE(backend.visibleComicModel()->rowCount(), 1);
        QVERIFY(!allBooks->property("highlighted").toBool());
        QVERIFY(!recentBooks->property("highlighted").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          allBooks->mapToScene(QPointF(allBooks->width() / 2, allBooks->height() / 2)).toPoint());
        QTRY_COMPARE(backend.visibleComicModel()->rowCount(), 20);
        QCOMPARE(filter->property("currentIndex").toInt(), 0);

        auto *slider = item(window->contentItem(), "thumbnailSize");
        auto *cover = item(window->contentItem(), "bookCover");
        auto *folders = item(window->contentItem(), "folderList");
        auto *scrollbar = item(window->contentItem(), "folderScrollBar");
        QVERIFY(slider);
        QTRY_VERIFY((cover = item(window->contentItem(), "bookCover")));
        QVERIFY(folders);
        QVERIFY(scrollbar);
        QTRY_VERIFY(scrollbar->isVisible());
        scrollbar->setProperty("position", 0.5);
        QTRY_VERIFY(folders->property("contentY").toReal() > 0);

        backend.setThumbnailSize(48);
        slider->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(backend.thumbnailSize(), 0);
        QVERIFY(!cover->isVisible());
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(backend.thumbnailSize(), 48);
        QTRY_VERIFY((cover = item(window->contentItem(), "bookCover")) && cover->isVisible());

        auto *bookList = item(window->contentItem(), "bookList");
        auto *bookScrollbar = item(window->contentItem(), "bookScrollBar");
        QVERIFY(bookList);
        QVERIFY(bookScrollbar);
        QTRY_VERIFY(bookScrollbar->isVisible());
        const QPoint start = bookScrollbar->mapToScene(
            QPointF(bookScrollbar->width() / 2,
                    bookScrollbar->height() * bookScrollbar->property("visualSize").toReal() / 2)).toPoint();
        const QPoint end = start + QPoint(0, qRound(bookScrollbar->height() * 0.6));
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, end, 100);
        QTRY_VERIFY(bookList->property("contentY").toReal() > 0);
        const qreal draggedPosition = bookList->property("contentY").toReal();
        backend.setCollection("/comics/book0.cbz", "Favorites");
        QTest::qWait(100);
        QVERIFY(bookList->property("contentY").toReal() >= draggedPosition - 1);
        const QPoint farther = start + QPoint(0, qRound(bookScrollbar->height() * 0.8));
        QTest::mouseMove(window, farther, 100);
        QTRY_VERIFY(bookList->property("contentY").toReal() > draggedPosition);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, farther);
        QTRY_VERIFY(bookList->property("contentY").toReal() > 0);
        search->setProperty("text", "no matching book");
        QTRY_VERIFY(!bookScrollbar->isVisible());
        search->setProperty("text", "");

        auto *panel = item(window->contentItem(), "libraryPanel");
        auto *mainArea = item(window->contentItem(), "mainArea");
        auto *hideLibrary = item(window->contentItem(), "hideLibraryButton");
        auto *showLibrary = item(window->contentItem(), "showLibraryButton");
        QVERIFY(panel);
        QVERIFY(mainArea);
        QVERIFY(hideLibrary);
        QVERIFY(showLibrary);
        window->setProperty("selectedPath", "/comics/book0.cbz");
        const qreal originalWidth = mainArea->width();
        const auto center = [](QQuickItem *control) {
            return control->mapToScene(QPointF(control->width() / 2, control->height() / 2)).toPoint();
        };
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(hideLibrary));
        QTRY_VERIFY(!panel->isVisible());
        QTRY_VERIFY(mainArea->width() > originalWidth + 300);
        QVERIFY(showLibrary->isVisible());
        QCOMPARE(window->property("selectedPath").toString(), QString("/comics/book0.cbz"));
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(!showLibrary->isVisible());
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(!panel->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(showLibrary));
        QTRY_VERIFY(panel->isVisible());

        auto *help = window->findChild<QObject *>("help");
        QTest::keyClick(window, '?');
        QTRY_VERIFY(help->property("opened").toBool());
        // A Material popup animates out: `opened` drops at once, `visible` after.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->property("visible").toBool());

        search->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_B);
        QTest::keyClick(window, Qt::Key_O);
        QTest::keyClick(window, Qt::Key_O);
        QTest::keyClick(window, Qt::Key_K);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(backend.searchTerms(), QStringList{"book"});
        QCOMPARE(search->property("text").toString(), QString());
        QTest::keyClick(window, Qt::Key_1);
        QCOMPARE(backend.visibleComics().size(), 11);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(backend.searchTerms(), (QStringList{"book", "1"}));
        auto *removeChip = item(window->contentItem(), "removeSearchChip");
        QVERIFY(removeChip);
        auto *chips = item(window->contentItem(), "searchChips");
        QVERIFY(chips);
        QTRY_VERIFY(chips->height() >= 32);
        QTRY_VERIFY(removeChip->width() > 0 && removeChip->height() > 0);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(removeChip));
        QTRY_COMPARE(backend.searchTerms(), QStringList{"1"});
        QCOMPARE(backend.visibleComics().size(), 11);
        removeChip = item(window->contentItem(), "removeSearchChip");
        QVERIFY(removeChip);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center(removeChip));
        QTRY_VERIFY(backend.searchTerms().isEmpty());
        QCOMPARE(backend.visibleComics().size(), 20);
        for (int i = 0; i < 10; ++i) {
            backend.setSearchText(QString("long term %1").arg(i));
            backend.commitSearchTerm();
        }
        auto *chipScrollbar = item(window->contentItem(), "searchChipScrollBar");
        QVERIFY(chips);
        QVERIFY(chipScrollbar);
        QTRY_VERIFY(chipScrollbar->isVisible());
        QVERIFY(chips->height() <= 104);
        chipScrollbar->setProperty("position", 0.5);
        QTRY_VERIFY(chips->property("contentY").toReal() > 0);
        window->resize(820, 560);
        QTRY_VERIFY(bookList->height() >= 60);

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(OmaComicsTests)
#include "omacomics_tests.moc"
