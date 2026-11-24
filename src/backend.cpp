#include "backend.h"
#include "privatestorage.h"

#include <QCollator>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QTimeZone>
#include <QTemporaryFile>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <sys/stat.h>

namespace {

constexpr qsizetype maxLibraryFileBytes = 20 * 1024 * 1024;
constexpr qsizetype maxArchiveListingBytes = 16 * 1024 * 1024;
constexpr qsizetype maxComicInfoBytes = 1024 * 1024;
constexpr qsizetype maxImageBytes = 64 * 1024 * 1024;

QJsonObject defaultPreferences() {
    return {{QStringLiteral("backupRetentionDays"), 30}, {QStringLiteral("startupBehavior"), 0},
            {QStringLiteral("scanOnStartup"), true}, {QStringLiteral("fitMode"), 0},
            {QStringLiteral("zoomStep"), 0}, {QStringLiteral("libraryVisible"), true},
            {QStringLiteral("thumbnailSize"), 88}, {QStringLiteral("doublePage"), false},
            {QStringLiteral("treeView"), false}, {QStringLiteral("arrowNavigation"), true},
            {QStringLiteral("wheelNavigation"), true}, {QStringLiteral("mouseNavigation"), true},
            {QStringLiteral("rememberApiKey"), true}, {QStringLiteral("useSeriesForLookup"), true}};
}

bool validPreference(const QString &name, const QJsonValue &value) {
    const QJsonValue fallback = defaultPreferences().value(name);
    if (fallback.isBool())
        return value.isBool();
    if (!fallback.isDouble() || !value.isDouble() || !std::isfinite(value.toDouble())
        || std::floor(value.toDouble()) != value.toDouble())
        return false;
    const double n = value.toDouble();
    if (name == QStringLiteral("backupRetentionDays"))
        return n == 0 || n == 7 || n == 30 || n == 90;
    if (name == QStringLiteral("thumbnailSize"))
        return n == 0 || (n >= 48 && n <= 192);
    return n >= 0 && n <= (name == QStringLiteral("zoomStep") ? 8 : 2);
}

QString localUrl(const QString &path) {
    return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
}

QString formatName(const QString &path) {
    return QFileInfo(path).suffix().toUpper();
}

QString fileStamp(const QFileInfo &info) {
    if (!info.exists() || !info.isFile() || !info.isReadable() || !info.lastModified().isValid())
        return {};
    return QString::number(info.size()) + QLatin1Char(':')
        + QString::number(info.lastModified().toMSecsSinceEpoch());
}

} // namespace

Backend::Backend(QObject *parent) : Backend({}, {}, parent) {}

Backend::Backend(const QString &dataDirectory, const QString &cacheDirectory, QObject *parent,
                 QNetworkAccessManager *network, const QString &credentialTool)
    : QObject(parent), m_visibleComicModel(this), m_comicTreeModel(this), m_scraper(this, network),
      m_credentials(dataDirectory.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("secret-tool"))
                                            : credentialTool, this), m_preferences(defaultPreferences()) {
    m_dataDirectory = dataDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : dataDirectory;
    m_cacheDirectory = cacheDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) : cacheDirectory;
    if (m_dataDirectory.isEmpty())
        m_dataDirectory = QDir::homePath() + QStringLiteral("/.local/share/omacomics");
    if (m_cacheDirectory.isEmpty())
        m_cacheDirectory = QDir::homePath() + QStringLiteral("/.cache/omacomics");
    m_libraryFile = m_dataDirectory + QStringLiteral("/library.json");
    m_currentStatusFile = m_dataDirectory + QStringLiteral("/current.json");
    m_backupDirectory = m_dataDirectory + QStringLiteral("/backups");
    for (const QString &directory : {m_dataDirectory, m_cacheDirectory, m_backupDirectory,
                                    m_cacheDirectory + QStringLiteral("/covers"),
                                    m_cacheDirectory + QStringLiteral("/pages"),
                                    m_cacheDirectory + QStringLiteral("/indexes")}) {
        if (!PrivateStorage::directory(directory)) {
            m_storageError = tr("Could not secure private comic storage: %1. Reading and indexing are disabled.").arg(directory);
            setStatus(m_storageError);
            return;
        }
    }
    m_storageReady = true;
    m_coverProcess.setChildProcessModifier([] { ::umask(0077); });
    m_pageProcess.setChildProcessModifier([] { ::umask(0077); });

    connect(this, &Backend::comicsChanged, this, &Backend::synchronizeVisibleComics);
    connect(this, &Backend::selectionChanged, this, [this] {
        m_comicTreeModel.synchronizeSelection(m_selectedComics);
    });
    connect(&m_credentials, &CredentialStore::loaded, this, [this](const QString &key) {
        if (m_preferences.value(QStringLiteral("rememberApiKey")).toBool())
            m_scraper.setApiKey(key);
    });
    connect(&m_backupCleanup, &QFutureWatcher<QString>::finished, this, [this] {
        m_backupCleanupActive = false;
        const QString error = m_backupCleanup.result();
        if (!error.isEmpty()) {
            m_lastPruned.clear();
            setStatus(error);
        }
        m_backupLocker.reset();
        if (error.isEmpty() && m_lastPruned.isEmpty())
            pruneBackups();
    });
    connect(&m_cacheWork, &QFutureWatcher<CacheResult>::finished, this, [this] {
        m_cacheActive = false;
        const CacheResult result = m_cacheWork.result();
        m_cacheBytes = result.bytes;
        if (m_clearingCache) {
            m_coverPaths.clear();
            m_coverAttempted.clear();
            m_clearingCache = false;
            emit comicsChanged();
            if (result.error.isEmpty())
                setStatus(tr("Image cache cleared. Library metadata and reading progress were kept."));
        }
        if (!result.error.isEmpty())
            setStatus(result.error);
        emit cacheChanged();
        m_cacheLocker.reset();
    });
    connect(&m_folderSearch, &QFutureWatcher<QList<QFileInfo>>::finished, this, &Backend::finishFolderSearch);
    connect(&m_scanProcess, &QProcess::readyReadStandardOutput, this, &Backend::processScanOutput);
    connect(&m_scanProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &Backend::finishScanProcess);
    connect(&m_scanProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        if (m_scanStage == ScanStage::PdfInfo)
            setStatus(tr("Could not read PDF %1. Check that Poppler is installed and the file is valid.")
                          .arg(QFileInfo(m_scanPath).fileName()));
        else
            setStatus(tr("Could not read comic archive %1. Check that 7-Zip is installed and the file is valid.")
                          .arg(QFileInfo(m_scanPath).fileName()));
        m_scanStage = ScanStage::Idle;
        finishIndexedFile(false);
    });
    connect(&m_coverProcess, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray output = m_coverProcess.readAllStandardOutput();
        if (!m_coverFile)
            return;
        m_coverBytes += output.size();
        if (m_coverBytes <= maxImageBytes && m_coverFile->write(output) == output.size())
            return;
        if (m_coverFile)
            m_coverProcess.kill();
    });
    connect(&m_coverProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &Backend::finishCoverExtraction);
    connect(&m_pageProcess, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray output = m_pageProcess.readAllStandardOutput();
        if (!m_pageFile)
            return;
        m_pageBytes += output.size();
        if (m_pageBytes <= maxImageBytes && m_pageFile->write(output) == output.size())
            return;
        if (m_pageFile)
            m_pageProcess.kill();
    });
    connect(&m_pageProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &Backend::finishPageExtraction);

    setStatus(tr("Add a library folder to get started."));
    loadState();
    m_preferences.insert(QStringLiteral("backupDirectory"), m_backupDirectory);
    loadSettings();
    m_preferencesLoaded = true;
    applyPreferences();
    writeCurrentStatus();
    if (dailyBackup()) {
        pruneBackups();
        if (m_preferences.value(QStringLiteral("scanOnStartup")).toBool() && !m_folderPaths.isEmpty())
            scanLibrary();
    }
}

bool Backend::storageReady() {
    if (!m_storageReady)
        setStatus(m_storageError);
    return m_storageReady;
}

QVariantList Backend::comics() const {
    QVariantList result;
    result.reserve(m_bookOrder.size());
    for (const QString &path : m_bookOrder)
        result.append(bookMap(path));
    return result;
}

QVariantList Backend::visibleComics() const {
    QVariantList result;
    QStringList needles = m_searchTerms;
    const QString draft = m_searchText.trimmed();
    if (!draft.isEmpty())
        needles.append(draft);
    for (const QString &path : m_bookOrder) {
        const QVariantMap book = bookMap(path);
        if (m_collectionFilter == QStringLiteral("__recent__")) {
            if (book.value(QStringLiteral("lastRead")).toString().isEmpty())
                continue;
        } else if (!m_collectionFilter.isEmpty()
                   && book.value(QStringLiteral("collection")).toString() != m_collectionFilter) {
            continue;
        }
        if (!needles.isEmpty()) {
            const QString searchable = book.value(QStringLiteral("title")).toString() + QLatin1Char(' ')
                + book.value(QStringLiteral("series")).toString() + QLatin1Char(' ')
                + book.value(QStringLiteral("writer")).toString() + QLatin1Char(' ')
                + book.value(QStringLiteral("publisher")).toString() + QLatin1Char(' ')
                + book.value(QStringLiteral("summary")).toString();
            if (!std::all_of(needles.cbegin(), needles.cend(), [&searchable](const QString &needle) {
                    return searchable.contains(needle, Qt::CaseInsensitive);
                }))
                continue;
        }
        result.append(book);
    }
    if (m_collectionFilter == QStringLiteral("__recent__")) {
        std::sort(result.begin(), result.end(), [](const QVariant &left, const QVariant &right) {
            return left.toMap().value(QStringLiteral("lastRead")).toString()
                > right.toMap().value(QStringLiteral("lastRead")).toString();
        });
    }
    return result;
}

void Backend::synchronizeVisibleComics() {
    if (m_synchronizingComics) {
        m_comicsSyncPending = true;
        return;
    }
    m_synchronizingComics = true;
    do {
        m_comicsSyncPending = false;
        const QVariantList visible = visibleComics();
        m_visibleComicModel.synchronize(visible);
        m_comicTreeModel.synchronize(visible, libraryFolders());
        QSet<QString> paths;
        for (const QVariant &comic : visible)
            paths.insert(comic.toMap().value(QStringLiteral("path")).toString());
        const QStringList previous = m_selectedComics;
        m_selectedComics.removeIf([&](const QString &path) { return !paths.contains(path); });
        if (previous != m_selectedComics)
            emit selectionChanged();
        m_comicTreeModel.synchronizeSelection(m_selectedComics);
    } while (m_comicsSyncPending);
    m_synchronizingComics = false;
}

QVariantList Backend::libraryFolders() const {
    QVariantList result;
    result.reserve(m_folderPaths.size());
    for (const QString &path : m_folderPaths)
        result.append(path);
    return result;
}

QVariantList Backend::collections() const {
    QStringList names;
    for (const QString &path : m_bookOrder) {
        const QString name = m_books.value(path).value(QStringLiteral("collection")).toString();
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    }
    names.sort(Qt::CaseInsensitive);
    QVariantList result;
    for (const QString &name : names)
        result.append(name);
    return result;
}

QVariantList Backend::bookmarks() const {
    QVariantList result;
    const QJsonArray stored = m_books.value(m_currentPath).value(QStringLiteral("bookmarks")).toArray();
    QList<int> pages;
    for (const QJsonValue &value : stored) {
        const int page = value.toInt();
        if (page > 0 && page <= m_pageCount && !pages.contains(page))
            pages.append(page);
    }
    std::sort(pages.begin(), pages.end());
    for (const int page : pages)
        result.append(page);
    return result;
}

QString Backend::currentTitle() const {
    return bookMap(m_currentPath).value(QStringLiteral("title")).toString();
}

bool Backend::currentBookmarked() const {
    const QJsonArray bookmarks = m_books.value(m_currentPath).value(QStringLiteral("bookmarks")).toArray();
    return std::any_of(bookmarks.cbegin(), bookmarks.cend(), [this](const QJsonValue &value) {
        return value.toInt() == m_currentPage;
    });
}

QVariantMap Backend::bookMap(const QString &path) const {
    QVariantMap map = m_books.value(path).toVariantMap();
    map.remove(QStringLiteral("sourceUrl"));
    const QVariantMap scraped = map.take(QStringLiteral("scraped")).toMap();
    const bool filenameTitle = map.value(QStringLiteral("titleFromFilename"),
                                         map.value(QStringLiteral("title")).toString() == bookTitle(path)).toBool();
    for (const QString &field : {QStringLiteral("title"), QStringLiteral("series"), QStringLiteral("number"),
                                QStringLiteral("year"), QStringLiteral("writer"), QStringLiteral("publisher"),
                                QStringLiteral("summary")}) {
        if (map.value(field).toString().trimmed().isEmpty()
            || (field == QStringLiteral("title") && filenameTitle)) {
            const QString value = scraped.value(field).toString().left(field == QStringLiteral("summary") ? 4000 : 256);
            if (!value.isEmpty())
                map.insert(field, value);
        }
    }
    const int comicVineId = scraped.value(QStringLiteral("comicVineId")).toInt();
    if (comicVineId > 0)
        map.insert(QStringLiteral("sourceUrl"),
                   QStringLiteral("https://comicvine.gamespot.com/issue/4000-%1/").arg(comicVineId));
    map.insert(QStringLiteral("path"), path);
    map.insert(QStringLiteral("cover"), localUrl(m_coverPaths.value(path)));
    return map;
}

QString Backend::bookTitle(const QString &path) const {
    return QFileInfo(path).completeBaseName();
}

bool Backend::isArchive(const QString &path) const {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("cbz") || suffix == QStringLiteral("cbr")
        || suffix == QStringLiteral("cb7") || suffix == QStringLiteral("cbt");
}

bool Backend::isImage(const QString &path) const {
    static const QStringList suffixes{QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                                      QStringLiteral("webp"), QStringLiteral("bmp"), QStringLiteral("gif"),
                                      QStringLiteral("tif"), QStringLiteral("tiff")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

QString Backend::pageCachePath(const QString &path, const QString &entry) const {
    const QByteArray key = QCryptographicHash::hash((path + QLatin1Char('\n')
                                                     + m_books.value(path).value(QStringLiteral("indexStamp")).toString()
                                                     + QLatin1Char('\n') + entry).toUtf8(),
                                                     QCryptographicHash::Sha256).toHex();
    QString suffix = QFileInfo(entry).suffix().toLower();
    if (suffix.isEmpty() || suffix.size() > 8)
        suffix = QStringLiteral("img");
    return m_cacheDirectory + QStringLiteral("/pages/") + QString::fromLatin1(key) + QLatin1Char('.') + suffix;
}

QString Backend::coverCachePath(const QString &path, const QString &entry) const {
    const QByteArray key = QCryptographicHash::hash((path + QStringLiteral("\ncover\n")
                                                     + m_books.value(path).value(QStringLiteral("indexStamp")).toString()
                                                     + QLatin1Char('\n') + entry).toUtf8(),
                                                     QCryptographicHash::Sha256).toHex();
    QString suffix = QFileInfo(entry).suffix().toLower();
    if (suffix.isEmpty() || suffix.size() > 8)
        suffix = QStringLiteral("png");
    return m_cacheDirectory + QStringLiteral("/covers/") + QString::fromLatin1(key) + QLatin1Char('.') + suffix;
}

QString Backend::pageIndexPath(const QString &path, const QString &stamp) const {
    const QByteArray key = QCryptographicHash::hash((path + QLatin1Char('\n') + stamp).toUtf8(),
                                                     QCryptographicHash::Sha256).toHex();
    return m_cacheDirectory + QStringLiteral("/indexes/") + QString::fromLatin1(key) + QStringLiteral(".json");
}

bool Backend::loadPageIndex(const QString &path) {
    const QString stamp = m_books.value(path).value(QStringLiteral("indexStamp")).toString();
    if (stamp.isEmpty() || stamp != fileStamp(QFileInfo(path)))
        return false;
    QFile file(pageIndexPath(path, stamp));
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxArchiveListingBytes)
        return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
        return false;
    QStringList pages;
    for (const QJsonValue &value : document.array()) {
        if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 4096)
            return false;
        pages.append(value.toString());
    }
    if (pages.isEmpty() || pages.size() > 50000
        || pages.size() != m_books.value(path).value(QStringLiteral("pageCount")).toInt())
        return false;
    m_pages.insert(path, pages);
    return true;
}

bool Backend::savePageIndex(const QString &path, const QString &stamp) {
    if (!storageReady())
        return false;
    QSaveFile file(pageIndexPath(path, stamp));
    const QByteArray contents = QJsonDocument(QJsonArray::fromStringList(m_pages.value(path))).toJson(QJsonDocument::Compact);
    if (!PrivateStorage::open(file) || file.write(contents) != contents.size() || !file.commit()) {
        setStatus(tr("The page index could not be saved: %1").arg(file.errorString()));
        return false;
    }
    return true;
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void Backend::setScanning(bool scanning) {
    if (m_scanning == scanning)
        return;
    m_scanning = scanning;
    if (scanning)
        m_scanLocker = std::make_unique<QEventLoopLocker>();
    emit scanningChanged();
    if (!scanning)
        m_scanLocker.reset();
}

void Backend::loadSettings() {
    QFile file(m_dataDirectory + QStringLiteral("/settings.json"));
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
        setStatus(tr("The settings file could not be loaded."));
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    const QJsonObject settings = document.object();
    const QJsonValue directory = settings.value(QStringLiteral("backupDirectory"));
    if (file.error() != QFileDevice::NoError || error.error != QJsonParseError::NoError
        || !document.isObject() || !directory.isString() || directory.toString().size() >= 4096
        || directory.toString().contains(QChar::Null)
        || !QDir::isAbsolutePath(directory.toString())) {
        setStatus(tr("The settings file could not be loaded."));
        return;
    }
    m_backupDirectory = QDir::cleanPath(directory.toString());
    m_preferences.insert(QStringLiteral("backupDirectory"), m_backupDirectory);
    const QJsonObject defaults = defaultPreferences();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!settings.contains(it.key()))
            continue;
        if (validPreference(it.key(), settings.value(it.key())))
            m_preferences.insert(it.key(), settings.value(it.key()));
        else
            setStatus(tr("The settings file contains an invalid preference: %1").arg(it.key()));
    }
}

bool Backend::savePreferences(const QJsonObject &preferences) {
    if (!storageReady())
        return false;
    QSaveFile file(m_dataDirectory + QStringLiteral("/settings.json"));
    const QByteArray data = QJsonDocument(preferences).toJson(QJsonDocument::Compact);
    if (!PrivateStorage::open(file) || file.write(data) != data.size() || !file.commit()) {
        setStatus(tr("Settings could not be saved: %1").arg(file.errorString()));
        return false;
    }
    m_preferences = preferences;
    return true;
}

void Backend::applyPreferences() {
    const bool layoutChanged = m_doublePage != m_preferences.value(QStringLiteral("doublePage")).toBool();
    m_thumbnailSize = m_preferences.value(QStringLiteral("thumbnailSize")).toInt(88);
    m_doublePage = m_preferences.value(QStringLiteral("doublePage")).toBool();
    m_treeView = m_preferences.value(QStringLiteral("treeView")).toBool();
    m_arrowNavigation = m_preferences.value(QStringLiteral("arrowNavigation")).toBool(true);
    m_wheelNavigation = m_preferences.value(QStringLiteral("wheelNavigation")).toBool(true);
    m_mouseNavigation = m_preferences.value(QStringLiteral("mouseNavigation")).toBool(true);
    emit preferencesChanged();
    emit readerSettingsChanged();
    emit thumbnailSizeChanged();
    if (layoutChanged && m_reading)
        startPageLoading();
    writeCurrentStatus();
}

bool Backend::setPreference(const QString &name, const QVariant &value) {
    const QJsonValue json = QJsonValue::fromVariant(value);
    if (!validPreference(name, json)) {
        setStatus(tr("Invalid setting: %1").arg(name));
        return false;
    }
    if (m_preferences.value(name) == json)
        return true;
    QJsonObject updated = m_preferences;
    updated.insert(name, json);
    if (!savePreferences(updated))
        return false;
    applyPreferences();
    if (name == QStringLiteral("backupRetentionDays")) {
        m_lastPruned.clear();
        if (dailyBackup())
            pruneBackups();
    }
    return true;
}

bool Backend::setBackupDirectory(const QString &directory) {
    if (!storageReady())
        return false;
    const QString path = directory.isEmpty() ? m_dataDirectory + QStringLiteral("/backups")
                                            : QDir::cleanPath(directory);
    if (directory.isEmpty() && !PrivateStorage::directory(path)) {
        setStatus(tr("Could not create the library backup folder."));
        return false;
    }
    if (!QDir::isAbsolutePath(path) || path.size() >= 4096 || path.contains(QChar::Null)
        || !QFileInfo(path).isDir()) {
        setStatus(tr("Choose a writable backup folder."));
        return false;
    }
    QTemporaryFile probe(QDir(path).filePath(QStringLiteral(".omacomics-write-XXXXXX")));
    if (!probe.open()) {
        setStatus(tr("Choose a writable backup folder."));
        return false;
    }
    QJsonObject updated = m_preferences;
    updated.insert(QStringLiteral("backupDirectory"), path);
    if (!savePreferences(updated))
        return false;
    m_backupDirectory = path;
    m_lastPruned.clear();
    emit backupDirectoryChanged();
    emit preferencesChanged();
    pruneBackups();
    setStatus(tr("Backup location updated."));
    return true;
}

void Backend::openFeedback(bool bugReport) {
    QUrl url(QStringLiteral("https://github.com/kelmankenberg/OmaComics/issues/new"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("template"),
                       bugReport ? QStringLiteral("bug_report.yml") : QStringLiteral("feature_request.yml"));
    url.setQuery(query);
    if (!QDesktopServices::openUrl(url))
        setStatus(tr("Could not open your browser. Open %1 manually.").arg(url.toString()));
}

void Backend::loadState() {
    QFile file(m_libraryFile);
    if (!file.exists())
        return;
    m_librarySavedAt = QFileInfo(file).lastModified();
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(tr("The library database could not be opened."));
        return;
    }
    if (file.size() > maxLibraryFileBytes) {
        setStatus(tr("The library database is too large to load."));
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        setStatus(tr("The library database is invalid."));
        return;
    }
    const QJsonObject root = document.object();
    for (const QJsonValue &value : root.value(QStringLiteral("folders")).toArray()) {
        const QString path = value.toString();
        if (!path.isEmpty() && path.size() < 4096 && !m_folderPaths.contains(path))
            m_folderPaths.append(path);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("books")).toArray()) {
        const QJsonObject book = value.toObject();
        const QString path = book.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || path.size() > 4096 || m_books.size() >= 20000)
            continue;
        m_books.insert(path, book);
        m_bookOrder.append(path);
    }
    m_currentPath = root.value(QStringLiteral("current")).toString();
    const int thumbnailSize = root.value(QStringLiteral("thumbnailSize")).toInt(88);
    m_thumbnailSize = thumbnailSize == 0 ? 0 : qBound(48, thumbnailSize, 192);
    m_doublePage = root.value(QStringLiteral("doublePage")).toBool();
    if (!m_preferencesLoaded && root.value(QStringLiteral("rightToLeft")).toBool()
        && m_books.contains(m_currentPath) && !m_books.value(m_currentPath).contains(QStringLiteral("manga"))) {
        QJsonObject book = m_books.value(m_currentPath);
        book.insert(QStringLiteral("manga"), true);
        m_books.insert(m_currentPath, book);
    }
    m_rightToLeft = m_books.value(m_currentPath).value(QStringLiteral("manga")).toBool();
    m_arrowNavigation = root.value(QStringLiteral("arrowNavigation")).toBool(true);
    m_wheelNavigation = root.value(QStringLiteral("wheelNavigation")).toBool(true);
    m_mouseNavigation = root.value(QStringLiteral("mouseNavigation")).toBool(true);
    m_treeView = root.value(QStringLiteral("treeView")).toBool();
    if (!m_preferencesLoaded) {
        const QJsonObject legacy{{QStringLiteral("thumbnailSize"), m_thumbnailSize},
                                 {QStringLiteral("doublePage"), m_doublePage}, {QStringLiteral("treeView"), m_treeView},
                                 {QStringLiteral("arrowNavigation"), m_arrowNavigation},
                                 {QStringLiteral("wheelNavigation"), m_wheelNavigation},
                                 {QStringLiteral("mouseNavigation"), m_mouseNavigation}};
        for (auto it = legacy.begin(); it != legacy.end(); ++it)
            m_preferences.insert(it.key(), it.value());
    } else {
        applyPreferences();
    }
    const QJsonObject current = m_books.value(m_currentPath);
    m_pageCount = qMax(0, current.value(QStringLiteral("pageCount")).toInt());
    m_currentPage = m_pageCount > 0 ? qBound(1, current.value(QStringLiteral("progress")).toInt(1), m_pageCount) : 0;
    emit libraryFoldersChanged();
    emit comicsChanged();
    emit thumbnailSizeChanged();
    emit readerSettingsChanged();
}

QByteArray Backend::libraryData() const {
    QJsonObject root;
    QJsonArray folders;
    for (const QString &path : m_folderPaths)
        folders.append(path);
    root.insert(QStringLiteral("folders"), folders);

    QJsonArray books;
    for (const QString &path : m_bookOrder) {
        QJsonObject book = m_books.value(path);
        book.insert(QStringLiteral("path"), path);
        books.append(book);
    }
    root.insert(QStringLiteral("books"), books);
    root.insert(QStringLiteral("current"), m_currentPath);
    root.insert(QStringLiteral("thumbnailSize"), m_thumbnailSize);
    root.insert(QStringLiteral("doublePage"), m_doublePage);
    root.insert(QStringLiteral("arrowNavigation"), m_arrowNavigation);
    root.insert(QStringLiteral("wheelNavigation"), m_wheelNavigation);
    root.insert(QStringLiteral("mouseNavigation"), m_mouseNavigation);
    root.insert(QStringLiteral("treeView"), m_treeView);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool Backend::writeLibraryFile(const QString &path, const QByteArray &data) {
    if (!storageReady())
        return false;
    if (data.size() > maxLibraryFileBytes) {
        setStatus(tr("The library is too large to save safely."));
        return false;
    }
    QSaveFile file(path);
    if (!PrivateStorage::open(file) || file.write(data) != data.size() || !file.commit()) {
        setStatus(tr("The library could not be saved: %1").arg(file.errorString()));
        return false;
    }
    return true;
}

void Backend::saveState() {
    if (!dailyBackup())
        return;
    writeLibraryFile(m_libraryFile, libraryData());
}

bool Backend::prepareBackupDirectory() {
    if (!storageReady())
        return false;
    if (m_backupDirectory == m_dataDirectory + QStringLiteral("/backups")
        && !PrivateStorage::directory(m_backupDirectory)) {
        setStatus(tr("Could not create the library backup folder."));
        return false;
    }
    if (!QFileInfo(m_backupDirectory).isDir()) {
        setStatus(tr("The backup folder is unavailable. Choose a new location in Settings."));
        return false;
    }
    return true;
}

bool Backend::dailyBackup() {
    if (m_books.isEmpty())
        return true;
    if (!prepareBackupDirectory())
        return false;
    const QString directory = m_backupDirectory;
    const QString name = QStringLiteral("daily-%1.json").arg(QDate::currentDate().toString(Qt::ISODate));
    const QString path = QDir(directory).filePath(name);
    if (!QFileInfo::exists(path) && !writeLibraryFile(path, libraryData()))
        return false;
    pruneBackups();
    return true;
}

void Backend::pruneBackups() {
    const int days = m_preferences.value(QStringLiteral("backupRetentionDays")).toInt(30);
    const QString token = m_backupDirectory + QDate::currentDate().toString(Qt::ISODate) + QString::number(days);
    if (m_lastPruned == token || m_backupCleanupActive || !m_storageReady)
        return;
    m_lastPruned = token;
    m_backupCleanupActive = true;
    m_backupLocker = std::make_unique<QEventLoopLocker>();
    const QString directory = m_backupDirectory;
    const QDate cutoff = QDate::currentDate().addDays(1 - days);
    m_backupCleanup.setFuture(QtConcurrent::run([directory, cutoff, days] {
        const QRegularExpression daily(QStringLiteral("^daily-(\\d{4}-\\d{2}-\\d{2})\\.json$"));
        const QRegularExpression safety(QStringLiteral("^safety-(\\d{8}-\\d{6}-\\d{3})-[a-f0-9]{32}\\.json$"));
        const QRegularExpression manual(QStringLiteral("^omacomics-\\d{8}-\\d{6}-\\d{3}-[a-f0-9]{32}\\.json$"));
        for (const QFileInfo &file : QDir(directory).entryInfoList(QDir::Files | QDir::NoSymLinks)) {
            QDate date;
            const auto dailyMatch = daily.match(file.fileName());
            const auto safetyMatch = safety.match(file.fileName());
            if (dailyMatch.hasMatch()) {
                date = QDate::fromString(dailyMatch.captured(1), Qt::ISODate);
                if (date.toString(Qt::ISODate) != dailyMatch.captured(1))
                    continue;
            } else if (safetyMatch.hasMatch()) {
                QDateTime stamp = QDateTime::fromString(safetyMatch.captured(1), QStringLiteral("yyyyMMdd-HHmmss-zzz"));
                if (stamp.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) != safetyMatch.captured(1))
                    continue;
                stamp.setTimeZone(QTimeZone::UTC);
                date = stamp.toLocalTime().date();
            }
            const bool recognized = date.isValid() || manual.match(file.fileName()).hasMatch();
            if (!recognized)
                continue;
            QFile candidate(file.filePath());
            if (!candidate.open(QIODevice::ReadOnly))
                return Backend::tr("Could not make a library backup private: %1").arg(file.fileName());
            if (candidate.size() > maxLibraryFileBytes)
                continue;
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(candidate.readAll(), &error);
            if (candidate.error() != QFileDevice::NoError || error.error != QJsonParseError::NoError
                || !document.isObject() || !document.object().value(QStringLiteral("books")).isArray()
                || !document.object().value(QStringLiteral("folders")).isArray())
                continue;
            candidate.close();
            if (!PrivateStorage::protectFile(file.filePath()))
                return Backend::tr("Could not make a library backup private: %1").arg(file.fileName());
            if (days == 0 || !date.isValid() || date >= cutoff)
                continue;
            if (!QFile::remove(file.filePath()))
                return Backend::tr("Could not remove an expired automatic backup: %1").arg(file.fileName());
        }
        return QString();
    }));
}

bool Backend::safetyBackup() {
    if (!prepareBackupDirectory())
        return false;
    const QString directory = m_backupDirectory;
    const QString name = QStringLiteral("safety-%1-%2.json")
                             .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
                                  QUuid::createUuid().toString(QUuid::Id128));
    if (!writeLibraryFile(QDir(directory).filePath(name), libraryData()))
        return false;
    pruneBackups();
    return true;
}

void Backend::backupLibrary(const QString &directory) {
    const QString destination = directory.isEmpty() ? m_backupDirectory : directory;
    if (directory.isEmpty() && !prepareBackupDirectory())
        return;
    const QFileInfo target(destination);
    if (!target.isDir() || !target.isWritable()) {
        setStatus(tr("Choose a writable backup folder."));
        return;
    }
    const QString name = QStringLiteral("omacomics-%1-%2.json")
                             .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
                                  QUuid::createUuid().toString(QUuid::Id128));
    if (writeLibraryFile(QDir(destination).filePath(name), libraryData()))
        setStatus(tr("Library backup saved to %1").arg(QDir(destination).filePath(name)));
}

void Backend::applyStartupBehavior() {
    if (m_currentPath.isEmpty() || !m_books.contains(m_currentPath))
        return;
    const int mode = m_preferences.value(QStringLiteral("startupBehavior")).toInt();
    if (mode == 1)
        emit previewRequested(m_currentPath);
    else if (mode == 2)
        resumeComic();
}

void Backend::loadComicVineKey() {
    if (m_preferences.value(QStringLiteral("rememberApiKey")).toBool())
        m_credentials.load();
}

void Backend::setComicVineKey(const QString &key) {
    if (m_credentials.busy()) {
        setStatus(tr("Wait for credential storage to finish."));
        return;
    }
    if (m_scraper.setApiKey(key) && m_scraper.configured()
        && m_preferences.value(QStringLiteral("rememberApiKey")).toBool())
        m_credentials.store(key.trimmed());
}

void Backend::forgetComicVineKey() {
    if (m_credentials.busy()) {
        setStatus(tr("Wait for credential storage to finish."));
        return;
    }
    m_scraper.setApiKey({});
    m_credentials.forget();
}

void Backend::refreshCacheSize() {
    if (!cacheBusy())
        startCacheWork(false);
}

void Backend::clearImageCache() {
    if (m_reading || m_scanning || cacheBusy() || !m_coverQueue.isEmpty()
        || m_coverProcess.state() != QProcess::NotRunning || !m_scheduledCoverIndexes.isEmpty()
        || m_pageProcess.state() != QProcess::NotRunning) {
        setStatus(tr("Close the reader and wait for background work before clearing the image cache."));
        return;
    }
    startCacheWork(true);
}

void Backend::startCacheWork(bool clear) {
    if (!storageReady())
        return;
    m_cacheActive = true;
    m_clearingCache = clear;
    m_cacheLocker = std::make_unique<QEventLoopLocker>();
    const QString root = m_cacheDirectory;
    m_cacheWork.setFuture(QtConcurrent::run([root, clear] {
        CacheResult result;
        const QRegularExpression generated(QStringLiteral("^[a-f0-9]{64}\\.[A-Za-z0-9]{1,8}$"));
        for (const QString &name : {QStringLiteral("covers"), QStringLiteral("pages")}) {
            const QDir directory(QDir(root).filePath(name));
            const QFileInfo info(directory.path());
            if (!info.isDir() || info.isSymLink() || !info.isReadable()) {
                result.error = Backend::tr("The image cache folder is unavailable.");
                return result;
            }
            for (const QFileInfo &file : directory.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
                if (!generated.match(file.fileName()).hasMatch())
                    continue;
                if (clear && !QFile::remove(file.filePath()))
                    result.error = Backend::tr("Some cached images could not be removed.");
                if (!clear || QFileInfo::exists(file.filePath()))
                    result.bytes += file.size();
            }
        }
        return result;
    }));
    emit cacheChanged();
}

void Backend::restoreLibrary(const QString &path) {
    if (m_scanning || m_reading) {
        setStatus(tr("Close the reader and wait for scanning to finish before restoring."));
        return;
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        setStatus(tr("Could not open the library backup: %1").arg(source.errorString()));
        return;
    }
    if (source.size() > maxLibraryFileBytes) {
        setStatus(tr("The library backup is invalid or too large."));
        return;
    }
    QJsonParseError error;
    const QByteArray data = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        setStatus(tr("Could not open the library backup: %1").arg(source.errorString()));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    const QJsonObject root = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || !root.value(QStringLiteral("folders")).isArray() || !root.value(QStringLiteral("books")).isArray()
        || root.value(QStringLiteral("books")).toArray().size() > 20000) {
        setStatus(tr("The library backup is invalid or too large."));
        return;
    }
    QSet<QString> paths;
    for (const QJsonValue &folder : root.value(QStringLiteral("folders")).toArray()) {
        if (!folder.isString() || folder.toString().isEmpty() || folder.toString().size() >= 4096) {
            setStatus(tr("The library backup is invalid or too large."));
            return;
        }
    }
    for (const QJsonValue &entry : root.value(QStringLiteral("books")).toArray()) {
        const QJsonValue value = entry.toObject().value(QStringLiteral("path"));
        const QString bookPath = value.toString();
        if (!entry.isObject() || !value.isString() || bookPath.isEmpty() || bookPath.size() > 4096
            || paths.contains(bookPath)) {
            setStatus(tr("The library backup is invalid or too large."));
            return;
        }
        paths.insert(bookPath);
    }
    if (!safetyBackup() || !writeLibraryFile(m_libraryFile, data))
        return;
    m_folderPaths.clear();
    m_books.clear();
    m_bookOrder.clear();
    m_pages.clear();
    m_coverPaths.clear();
    m_coverAttempted.clear();
    m_pendingCovers.clear();
    m_coverQueue.clear();
    m_pendingOpenPath.clear();
    m_deferredIndexes.clear();
    if (!m_pendingOpenPath.isEmpty() && m_scanQueue.removeAll(m_pendingOpenPath) > 0)
        m_scanQueue.prepend(m_pendingOpenPath);
    m_currentPath.clear();
    m_currentPage = 0;
    m_pageCount = 0;
    loadState();
    emit readerChanged();
    writeCurrentStatus();
    setStatus(tr("Library restored from backup."));
    if (!m_folderPaths.isEmpty())
        scanLibrary();
}

void Backend::writeCurrentStatus() {
    if (!storageReady())
        return;
    QJsonObject status;
    status.insert(QStringLiteral("title"), currentTitle().left(256));
    status.insert(QStringLiteral("path"), m_currentPath.left(4096));
    status.insert(QStringLiteral("page"), m_currentPage);
    status.insert(QStringLiteral("pageCount"), m_pageCount);
    status.insert(QStringLiteral("progress"), m_pageCount > 0
                  ? qBound(0, int(qint64(m_currentPage) * 100 / m_pageCount), 100) : 0);
    const bool finalPageVisible = m_currentPage >= m_pageCount
        || (m_doublePage && m_currentPage > 1 && m_currentPage >= m_pageCount - 1);
    status.insert(QStringLiteral("inProgress"), m_books.contains(m_currentPath) && m_currentPage > 0
                  && m_pageCount > 0 && !finalPageVisible);

    QSaveFile file(m_currentStatusFile);
    if (!PrivateStorage::open(file)
        || file.write(QJsonDocument(status).toJson(QJsonDocument::Compact)) < 0
        || !file.commit())
        setStatus(tr("Reading progress could not be shared with the bar widget: %1").arg(file.errorString()));
}

void Backend::updateBook(const QString &path, const QJsonObject &values) {
    QJsonObject book = m_books.value(path);
    for (auto it = values.begin(); it != values.end(); ++it)
        book.insert(it.key(), it.value());
    if (!book.contains(QStringLiteral("title")) || book.value(QStringLiteral("title")).toString().isEmpty())
        book.insert(QStringLiteral("title"), bookTitle(path));
    book.insert(QStringLiteral("path"), path);
    m_books.insert(path, book);
    if (!m_bookOrder.contains(path))
        m_bookOrder.append(path);

    QCollator collator;
    collator.setNumericMode(true);
    std::sort(m_bookOrder.begin(), m_bookOrder.end(), [this, &collator](const QString &left, const QString &right) {
        return collator.compare(m_books.value(left).value(QStringLiteral("title")).toString(),
                                m_books.value(right).value(QStringLiteral("title")).toString()) < 0;
    });
    emit comicsChanged();
}

void Backend::addLibraryFolder(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir() || !info.isReadable()) {
        setStatus(tr("Choose a readable folder."));
        return;
    }
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        setStatus(tr("The selected folder could not be resolved."));
        return;
    }
    if (m_folderPaths.contains(canonical)) {
        setStatus(tr("That folder is already in the library."));
        return;
    }
    m_folderPaths.append(canonical);
    emit libraryFoldersChanged();
    saveState();
    if (m_scanning)
        m_rescanAfterCurrent = true;
    else
        scanLibrary();
}

void Backend::removeLibraryFolder(const QString &path) {
    if (m_scanning) {
        setStatus(tr("Wait for the library scan to finish before removing a folder."));
        return;
    }
    if (!m_folderPaths.contains(path))
        return;
    if (!safetyBackup())
        return;
    if (!m_folderPaths.removeOne(path))
        return;
    const QString prefix = QDir::cleanPath(path) + QDir::separator();
    for (auto it = m_books.begin(); it != m_books.end();) {
        if (it.key() == path || it.key().startsWith(prefix)) {
            m_pages.remove(it.key());
            m_coverPaths.remove(it.key());
            it = m_books.erase(it);
        } else {
            ++it;
        }
    }
    m_bookOrder.removeIf([this](const QString &bookPath) { return !m_books.contains(bookPath); });
    if (!m_currentPath.isEmpty() && !m_books.contains(m_currentPath)) {
        closeReader();
        m_currentPath.clear();
        m_currentPage = 0;
        m_pageCount = 0;
        emit readerChanged();
        writeCurrentStatus();
    }
    emit libraryFoldersChanged();
    emit comicsChanged();
    saveState();
    scanLibrary();
}

void Backend::removeFromRecent(const QString &path) {
    if (!m_books.contains(path) || !m_books.value(path).contains(QStringLiteral("lastRead")))
        return;
    if (!safetyBackup())
        return;
    QJsonObject book = m_books.value(path);
    book.remove(QStringLiteral("lastRead"));
    m_books.insert(path, book);
    emit comicsChanged();
    saveState();
}

void Backend::scanLibrary() {
    if (!storageReady())
        return;
    if (m_clearingCache) {
        setStatus(tr("Wait for image cache cleanup to finish."));
        return;
    }
    if (m_scanning) {
        setStatus(tr("A library scan is already running."));
        return;
    }
    if (m_folderPaths.isEmpty()) {
        setStatus(tr("Add a library folder to get started."));
        return;
    }
    setScanning(true);
    m_scanError.clear();
    setStatus(tr("Finding comics and PDFs…"));
    m_coverAttempted.clear();
    m_discovering = true;
    const QStringList folders = m_folderPaths;
    m_folderSearch.setFuture(QtConcurrent::run([folders] {
        QList<QFileInfo> found;
        QSet<QString> paths;
        const QStringList patterns{QStringLiteral("*.cbz"), QStringLiteral("*.cbr"),
                                   QStringLiteral("*.cb7"), QStringLiteral("*.cbt"),
                                   QStringLiteral("*.pdf")};
        for (const QString &folder : folders) {
            QDirIterator iterator(folder, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                const QString path = iterator.next();
                const QFileInfo info = iterator.fileInfo();
                if (!paths.contains(path) && QDir::match(patterns, info.fileName())) {
                    paths.insert(path);
                    found.append(info);
                }
            }
        }
        std::sort(found.begin(), found.end(), [](const QFileInfo &left, const QFileInfo &right) {
            return QString::compare(left.absoluteFilePath(), right.absoluteFilePath(), Qt::CaseInsensitive) < 0;
        });
        return found;
    }));
}

void Backend::finishFolderSearch() {
    m_discovering = false;
    m_scanQueue.clear();
    for (const QFileInfo &info : m_folderSearch.result()) {
        const QString path = info.absoluteFilePath();
        const QString stamp = fileStamp(info);
        QJsonObject book = m_books.value(path);
        const QString previous = book.value(QStringLiteral("indexStamp")).toString();
        if (!stamp.isEmpty() && !book.isEmpty() && previous.isEmpty()
            && book.value(QStringLiteral("pageCount")).toInt() > 0
            && m_librarySavedAt.isValid() && info.lastModified() <= m_librarySavedAt) {
            book.insert(QStringLiteral("indexStamp"), stamp);
            book.insert(QStringLiteral("indexProvisional"), true);
            m_books.insert(path, book);
        } else if (stamp.isEmpty() || book.isEmpty() || stamp != previous) {
            m_pages.remove(path);
            m_coverPaths.remove(path);
            m_coverAttempted.remove(path);
            m_scanQueue.append(path);
        }
    }
    for (const QString &path : std::as_const(m_deferredIndexes)) {
        if (path == m_pendingOpenPath) {
            m_scanQueue.removeAll(path);
            m_scanQueue.prepend(path);
        } else if (!m_scanQueue.contains(path)) {
            m_scanQueue.append(path);
        }
    }
    m_deferredIndexes.clear();
    m_scanOutput.clear();
    scanNextFile();
    emit comicsChanged();
}

void Backend::requestIndex(const QString &path, bool priority) {
    if (m_discovering) {
        if (!m_deferredIndexes.contains(path))
            m_deferredIndexes.append(path);
        return;
    }
    if (m_scanning && m_scanPath == path)
        return;
    if (priority)
        m_scanQueue.removeAll(path);
    if (!m_scanQueue.contains(path)) {
        if (priority)
            m_scanQueue.prepend(path);
        else
            m_scanQueue.append(path);
    }
    if (!m_scanning) {
        m_scanError.clear();
        setScanning(true);
        scanNextFile();
    }
}

void Backend::scanNextFile() {
    if (m_scanProcess.state() != QProcess::NotRunning)
        return;
    if (m_scanQueue.isEmpty()) {
        setScanning(false);
        setStatus(m_scanError.isEmpty() ? tr("%n books in your library.", nullptr, m_bookOrder.size()) : m_scanError);
        emit comicsChanged();
        saveState();
        if (m_rescanAfterCurrent) {
            m_rescanAfterCurrent = false;
            scanLibrary();
            return;
        }
        if (!m_pendingOpenPath.isEmpty()) {
            const QString path = m_pendingOpenPath;
            m_pendingOpenPath.clear();
            openComic(path);
        }
        return;
    }

    m_scanPath = m_scanQueue.takeFirst();
    m_scanStamp = fileStamp(QFileInfo(m_scanPath));
    m_scannedValues = {};
    if (m_scanStamp.isEmpty()) {
        setStatus(tr("Choose a readable CBZ, CBR, CB7, CBT, or PDF file."));
        finishIndexedFile(false);
        return;
    }
    m_scanOutput.clear();
    m_scanOutputTooLarge = false;
    if (isArchive(m_scanPath)) {
        m_scanStage = ScanStage::ArchiveList;
        setStatus(tr("Indexing %1").arg(QFileInfo(m_scanPath).fileName()));
        m_scanProcess.start(QStringLiteral("7z"),
                            {QStringLiteral("l"), QStringLiteral("-slt"), QStringLiteral("-ba"),
                             QStringLiteral("-bd"), m_scanPath});
    } else {
        m_scanStage = ScanStage::PdfInfo;
        setStatus(tr("Reading PDF information for %1").arg(QFileInfo(m_scanPath).fileName()));
        m_scanProcess.start(QStringLiteral("pdfinfo"), {m_scanPath});
    }
}

void Backend::finishIndexedFile(bool success) {
    const QString path = m_scanPath;
    if (success && fileStamp(QFileInfo(path)) != m_scanStamp) {
        success = false;
        m_pages.remove(path);
        setStatus(tr("The file changed while indexing: %1").arg(QFileInfo(path).fileName()));
    }
    if (success) {
        QJsonObject book = m_books.value(path);
        for (const QString &key : {QStringLiteral("title"), QStringLiteral("format"), QStringLiteral("pageCount"),
                                   QStringLiteral("series"), QStringLiteral("number"), QStringLiteral("summary"),
                                   QStringLiteral("writer"), QStringLiteral("publisher"), QStringLiteral("year"),
                                   QStringLiteral("genre")})
            book.remove(key);
        book.insert(QStringLiteral("indexStamp"), m_scanStamp);
        book.remove(QStringLiteral("indexProvisional"));
        m_books.insert(path, book);
        updateBook(path, m_scannedValues);
        if (isArchive(path) && !savePageIndex(path, m_scanStamp))
            m_scanError = m_status;
    } else {
        m_pages.remove(path);
        m_coverAttempted.insert(path);
        m_scanError = m_status;
    }
    m_scanPath.clear();
    if (m_pendingCovers.remove(path) && success)
        ensureCover(path);
    emit comicsChanged();
    if (m_pendingOpenPath == path) {
        m_pendingOpenPath.clear();
        if (success)
            openComic(path);
    }
    if (success)
        scanNextFile();
    else
        QTimer::singleShot(0, this, &Backend::scanNextFile);
}

void Backend::processScanOutput() {
    const QByteArray output = m_scanProcess.readAllStandardOutput();
    const qsizetype limit = m_scanStage == ScanStage::ArchiveMetadata ? maxComicInfoBytes : maxArchiveListingBytes;
    if (m_scanOutput.size() + output.size() > limit) {
        m_scanOutputTooLarge = true;
        m_scanProcess.kill();
        return;
    }
    m_scanOutput.append(output);
}

void Backend::finishScanProcess(int exitCode, QProcess::ExitStatus exitStatus) {
    processScanOutput();
    if (m_scanOutputTooLarge) {
        setStatus(tr("An archive entry exceeded the safe size limit: %1").arg(QFileInfo(m_scanPath).fileName()));
        m_scanStage = ScanStage::Idle;
        finishIndexedFile(false);
        return;
    }

    const bool success = exitCode == 0 && exitStatus == QProcess::NormalExit;
    const ScanStage stage = m_scanStage;
    m_scanStage = ScanStage::Idle;
    if (stage == ScanStage::ArchiveList) {
        if (success) {
            parseArchiveListing(m_scanOutput);
            if (m_currentPages.isEmpty()) {
                finishIndexedFile(false);
                return;
            }
            if (!m_metadataEntry.isEmpty()) {
                m_scanOutput.clear();
                m_scanStage = ScanStage::ArchiveMetadata;
                m_scanProcess.start(QStringLiteral("7z"),
                                    {QStringLiteral("x"), QStringLiteral("-so"), QStringLiteral("-y"),
                                     QStringLiteral("-bd"), m_scanPath, m_metadataEntry});
                return;
            }
        } else {
            setStatus(tr("Could not read comic archive %1. Check that 7-Zip is installed and the file is valid.")
                          .arg(QFileInfo(m_scanPath).fileName()));
        }
    } else if (stage == ScanStage::ArchiveMetadata) {
        if (success) {
            if (!parseComicInfo(m_scanOutput)) {
                finishIndexedFile(false);
                return;
            }
        } else {
            setStatus(tr("ComicInfo.xml could not be read from %1.").arg(QFileInfo(m_scanPath).fileName()));
        }
    } else if (stage == ScanStage::PdfInfo) {
        if (success) {
            const QString output = QString::fromLocal8Bit(m_scanOutput);
            const QRegularExpression pagesPattern(QStringLiteral("^Pages:\\s+(\\d+)"),
                                                   QRegularExpression::MultilineOption);
            const auto match = pagesPattern.match(output);
            bool valid = false;
            const int pages = match.captured(1).toInt(&valid);
            if (valid && pages > 0 && pages <= 100000) {
                QJsonObject values;
                values.insert(QStringLiteral("title"), bookTitle(m_scanPath));
                values.insert(QStringLiteral("titleFromFilename"), true);
                values.insert(QStringLiteral("format"), QStringLiteral("PDF"));
                values.insert(QStringLiteral("pageCount"), pages);
                m_scannedValues = values;
            } else {
                setStatus(tr("The PDF page count could not be determined for %1.")
                              .arg(QFileInfo(m_scanPath).fileName()));
                finishIndexedFile(false);
                return;
            }
        } else {
            setStatus(tr("Could not read PDF %1. Check that Poppler is installed and the file is valid.")
                          .arg(QFileInfo(m_scanPath).fileName()));
        }
    }
    finishIndexedFile(success);
}

void Backend::parseArchiveListing(const QByteArray &output) {
    m_currentPages.clear();
    m_metadataEntry.clear();
    QString currentPath;
    bool directory = false;
    auto consume = [this, &currentPath, &directory] {
        if (currentPath.isEmpty() || directory || currentPath.size() > 4096
            || currentPath.startsWith('/') || currentPath.contains(QStringLiteral("../")))
            return;
        if (currentPath.compare(QStringLiteral("ComicInfo.xml"), Qt::CaseInsensitive) == 0
            || currentPath.endsWith(QStringLiteral("/ComicInfo.xml"), Qt::CaseInsensitive))
            m_metadataEntry = currentPath;
        if (isImage(currentPath))
            m_currentPages.append(currentPath);
    };
    const QString text = QString::fromUtf8(output);
    for (const QString &line : text.split(u'\n')) {
        if (line.startsWith(QStringLiteral("Path = "))) {
            consume();
            currentPath = line.mid(7).trimmed();
            directory = false;
        } else if (line == QStringLiteral("Folder = +")) {
            directory = true;
        }
    }
    consume();

    QCollator collator;
    collator.setNumericMode(true);
    std::sort(m_currentPages.begin(), m_currentPages.end(), [&collator](const QString &left, const QString &right) {
        return collator.compare(left, right) < 0;
    });
    if (m_currentPages.size() > 50000)
        m_currentPages.resize(50000);
    m_pages.insert(m_scanPath, m_currentPages);

    QJsonObject values;
    values.insert(QStringLiteral("title"), bookTitle(m_scanPath));
    values.insert(QStringLiteral("titleFromFilename"), true);
    values.insert(QStringLiteral("format"), formatName(m_scanPath));
    values.insert(QStringLiteral("pageCount"), m_currentPages.size());
    m_scannedValues = values;
    if (m_currentPages.isEmpty())
        setStatus(tr("No readable comic pages were found in %1.").arg(QFileInfo(m_scanPath).fileName()));
}

bool Backend::parseComicInfo(const QByteArray &xml) {
    QXmlStreamReader reader(xml);
    QJsonObject values;
    static const QHash<QString, QString> fields{
        {QStringLiteral("Title"), QStringLiteral("title")},
        {QStringLiteral("Series"), QStringLiteral("series")},
        {QStringLiteral("Number"), QStringLiteral("number")},
        {QStringLiteral("Summary"), QStringLiteral("summary")},
        {QStringLiteral("Writer"), QStringLiteral("writer")},
        {QStringLiteral("Publisher"), QStringLiteral("publisher")},
        {QStringLiteral("Year"), QStringLiteral("year")},
        {QStringLiteral("Genre"), QStringLiteral("genre")},
    };
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        const QString key = fields.value(reader.name().toString());
        if (key.isEmpty())
            continue;
        const QString text = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        if (!text.isEmpty())
            values.insert(key, text.left(key == QStringLiteral("summary") ? 4000 : 256));
    }
    if (reader.hasError()) {
        setStatus(tr("ComicInfo.xml is malformed in %1.").arg(QFileInfo(m_scanPath).fileName()));
        return false;
    }
    for (auto it = values.begin(); it != values.end(); ++it)
        m_scannedValues.insert(it.key(), it.value());
    if (values.contains(QStringLiteral("title")))
        m_scannedValues.insert(QStringLiteral("titleFromFilename"), false);
    return true;
}

void Backend::setSearchText(const QString &text) {
    const QString limited = text.left(256);
    if (m_searchText == limited)
        return;
    m_searchText = limited;
    emit comicsChanged();
}

void Backend::commitSearchTerm() {
    const QString term = m_searchText.trimmed();
    if (term.isEmpty())
        return;
    const bool exists = std::any_of(m_searchTerms.cbegin(), m_searchTerms.cend(), [&term](const QString &current) {
        return current.compare(term, Qt::CaseInsensitive) == 0;
    });
    m_searchText.clear();
    if (!exists) {
        m_searchTerms.append(term);
        emit searchTermsChanged();
    }
    emit comicsChanged();
}

void Backend::removeSearchTerm(int index) {
    if (index < 0 || index >= m_searchTerms.size())
        return;
    m_searchTerms.removeAt(index);
    emit searchTermsChanged();
    emit comicsChanged();
}

void Backend::setCollectionFilter(const QString &collection) {
    if (m_collectionFilter == collection)
        return;
    m_collectionFilter = collection == QStringLiteral("__recent__")
        ? collection : collection.left(100);
    emit comicsChanged();
}

void Backend::setTreeView(bool tree) {
    setPreference(QStringLiteral("treeView"), tree);
}

void Backend::selectComic(const QString &path, bool selected) {
    if (!m_books.contains(path)) {
        setStatus(tr("The selected comic is no longer in the library."));
        return;
    }
    if (selected == m_selectedComics.contains(path))
        return;
    if (selected)
        m_selectedComics.append(path);
    else
        m_selectedComics.removeAll(path);
    emit selectionChanged();
}

void Backend::selectFolder(const QString &path, bool selected) {
    const QString folder = QDir::cleanPath(path);
    const QString prefix = folder.endsWith('/') ? folder : folder + '/';
    const QStringList previous = m_selectedComics;
    QSet<QString> selection(m_selectedComics.cbegin(), m_selectedComics.cend());
    for (const QVariant &comic : visibleComics()) {
        const QString comicPath = comic.toMap().value(QStringLiteral("path")).toString();
        if (!comicPath.startsWith(prefix))
            continue;
        if (selected && !selection.contains(comicPath)) {
            m_selectedComics.append(comicPath);
            selection.insert(comicPath);
        } else if (!selected) {
            selection.remove(comicPath);
        }
    }
    if (!selected)
        m_selectedComics.removeIf([&](const QString &comicPath) { return !selection.contains(comicPath); });
    if (previous != m_selectedComics)
        emit selectionChanged();
}

void Backend::selectAllVisible() {
    m_selectedComics.clear();
    for (const QVariant &comic : visibleComics())
        m_selectedComics.append(comic.toMap().value(QStringLiteral("path")).toString());
    emit selectionChanged();
}

void Backend::clearSelection() {
    if (m_selectedComics.isEmpty())
        return;
    m_selectedComics.clear();
    emit selectionChanged();
}

QString Backend::scrapeQuery(const QString &path) const {
    const QVariantMap comic = bookMap(path);
    const QString series = comic.value(QStringLiteral("series")).toString();
    const QString title = comic.value(QStringLiteral("title")).toString();
    return (series.isEmpty() || !m_preferences.value(QStringLiteral("useSeriesForLookup")).toBool() ? title : series + QLatin1Char(' ')
            + comic.value(QStringLiteral("number")).toString()).simplified().left(200);
}

QVariantList Backend::missingMetadata(const QString &path) const {
    const QVariantMap existing = bookMap(path);
    const QVariantMap metadata = m_scraper.metadata();
    const QList<QPair<QString, QString>> fields{
        {QStringLiteral("title"), tr("Title")}, {QStringLiteral("series"), tr("Series")},
        {QStringLiteral("number"), tr("Issue number")}, {QStringLiteral("year"), tr("Year")},
        {QStringLiteral("writer"), tr("Writer")}, {QStringLiteral("publisher"), tr("Publisher")},
        {QStringLiteral("summary"), tr("Summary")}};
    QVariantList missing;
    if (!m_books.contains(path))
        return missing;
    for (const auto &field : fields) {
        const QString value = metadata.value(field.first).toString().trimmed();
        const QString current = existing.value(field.first).toString().trimmed();
        const QJsonObject local = m_books.value(path);
        const bool filenameTitle = local.value(QStringLiteral("titleFromFilename"))
            .toBool(local.value(QStringLiteral("title")).toString() == bookTitle(path));
        if (!value.isEmpty() && (current.isEmpty()
            || (field.first == QStringLiteral("title") && filenameTitle && current == bookTitle(path))))
            missing.append(QVariantMap{{QStringLiteral("key"), field.first},
                                       {QStringLiteral("label"), field.second},
                                       {QStringLiteral("value"), value}});
    }
    return missing;
}

bool Backend::applyScrapedMetadata(const QString &path) {
    if (!m_books.contains(path) || m_scraper.busy()
        || m_scraper.metadata().value(QStringLiteral("comicVineId")).toInt() <= 0) {
        setStatus(tr("Choose a Comic Vine match for a library comic before applying metadata."));
        return false;
    }
    const QVariantList missing = missingMetadata(path);
    if (missing.isEmpty()) {
        setStatus(tr("No missing fields. Existing metadata will be kept."));
        return true;
    }
    if (!safetyBackup())
        return false;
    const QJsonObject original = m_books.value(path);
    QJsonObject book = original;
    QJsonObject scraped = book.value(QStringLiteral("scraped")).toObject();
    for (const QVariant &field : missing) {
        const QVariantMap item = field.toMap();
        scraped.insert(item.value(QStringLiteral("key")).toString(),
                       item.value(QStringLiteral("value")).toString());
    }
    scraped.insert(QStringLiteral("comicVineId"), m_scraper.metadata().value(QStringLiteral("comicVineId")).toInt());
    book.insert(QStringLiteral("scraped"), scraped);
    m_books.insert(path, book);
    if (!writeLibraryFile(m_libraryFile, libraryData())) {
        m_books.insert(path, original);
        return false;
    }
    emit comicsChanged();
    if (path == m_currentPath) {
        emit readerChanged();
        writeCurrentStatus();
    }
    setStatus(tr("Comic Vine metadata saved. Original comic files were not changed."));
    return true;
}

void Backend::setThumbnailSize(int size) {
    const int bounded = size == 0 ? 0 : qBound(48, size, 192);
    setPreference(QStringLiteral("thumbnailSize"), bounded);
}

void Backend::setDoublePage(bool enabled) {
    setPreference(QStringLiteral("doublePage"), enabled);
}

void Backend::setRightToLeft(bool enabled) {
    setMangaForPaths({m_currentPath}, enabled);
}

void Backend::setSelectedManga(bool enabled) {
    setMangaForPaths(m_selectedComics, enabled);
}

void Backend::setMangaForPaths(const QStringList &paths, bool enabled) {
    if (paths.isEmpty() || std::any_of(paths.cbegin(), paths.cend(), [this](const QString &path) {
            return !m_books.contains(path);
        })) {
        setStatus(tr("Select comics or open a comic before changing manga direction."));
        return;
    }
    QHash<QString, QJsonObject> previous;
    for (const QString &path : paths) {
        previous.insert(path, m_books.value(path));
        QJsonObject book = m_books.value(path);
        book.insert(QStringLiteral("manga"), enabled);
        m_books.insert(path, book);
    }
    if (!writeLibraryFile(m_libraryFile, libraryData())) {
        for (auto it = previous.begin(); it != previous.end(); ++it)
            m_books.insert(it.key(), it.value());
        return;
    }
    m_rightToLeft = m_books.value(m_currentPath).value(QStringLiteral("manga")).toBool();
    emit readerSettingsChanged();
    emit comicsChanged();
}

void Backend::setArrowNavigation(bool enabled) {
    setPreference(QStringLiteral("arrowNavigation"), enabled);
}

void Backend::setWheelNavigation(bool enabled) {
    setPreference(QStringLiteral("wheelNavigation"), enabled);
}

void Backend::setMouseNavigation(bool enabled) {
    setPreference(QStringLiteral("mouseNavigation"), enabled);
}

void Backend::setCollection(const QString &path, const QString &collection) {
    if (!m_books.contains(path))
        return;
    const QString cleaned = collection.trimmed().left(100);
    if (cleaned.contains(QChar::Null))
        return;
    QJsonObject book = m_books.value(path);
    book.insert(QStringLiteral("collection"), cleaned);
    m_books.insert(path, book);
    emit comicsChanged();
    saveState();
}

void Backend::ensureCover(const QString &path) {
    if (!storageReady())
        return;
    if (m_clearingCache || m_coverPaths.contains(path) || m_coverAttempted.contains(path) || !m_books.contains(path))
        return;
    if (m_scanning && m_scanPath == path) {
        m_pendingCovers.insert(path);
        return;
    }
    if (isArchive(path) && !m_pages.contains(path) && !loadPageIndex(path)) {
        m_pendingCovers.insert(path);
        if (!m_scheduledCoverIndexes.contains(path)) {
            m_scheduledCoverIndexes.insert(path);
            QTimer::singleShot(0, this, [this, path] {
                m_scheduledCoverIndexes.remove(path);
                if (m_books.contains(path)) {
                    if (m_pages.contains(path))
                        ensureCover(path);
                    else
                        requestIndex(path, false);
                }
            });
        }
        return;
    }
    m_coverQueue.enqueue(path);
    m_coverAttempted.insert(path);
    startCoverExtraction();
}

void Backend::startCoverExtraction() {
    if (m_coverProcess.state() != QProcess::NotRunning || m_coverQueue.isEmpty())
        return;
    m_currentCoverPath = m_coverQueue.dequeue();
    m_coverEntry.clear();
    const QString suffix = QFileInfo(m_currentCoverPath).suffix().toLower();
    if (suffix == QStringLiteral("pdf")) {
        m_coverOutputPath = coverCachePath(m_currentCoverPath, QStringLiteral("cover.png"));
        if (QFileInfo::exists(m_coverOutputPath)) {
            m_coverPaths.insert(m_currentCoverPath, m_coverOutputPath);
            emit comicsChanged();
            startCoverExtraction();
            return;
        }
        m_pdfOutputPrefix = m_coverOutputPath.left(m_coverOutputPath.size() - 4);
        m_coverProcess.start(QStringLiteral("pdftoppm"),
                             {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"), QStringLiteral("1"),
                              QStringLiteral("-singlefile"), QStringLiteral("-scale-to"), QStringLiteral("320"),
                              QStringLiteral("-png"), m_currentCoverPath, m_pdfOutputPrefix});
        return;
    }
    const QStringList pages = m_pages.value(m_currentCoverPath);
    if (pages.isEmpty()) {
        startCoverExtraction();
        return;
    }
    m_coverEntry = pages.first();
    m_coverOutputPath = coverCachePath(m_currentCoverPath, m_coverEntry);
    if (QFileInfo::exists(m_coverOutputPath)) {
        m_coverPaths.insert(m_currentCoverPath, m_coverOutputPath);
        emit comicsChanged();
        startCoverExtraction();
        return;
    }
    m_coverFile = std::make_unique<QSaveFile>(m_coverOutputPath);
    m_coverBytes = 0;
    if (!PrivateStorage::open(*m_coverFile)) {
        m_coverFile.reset();
        setStatus(tr("Could not create a cover thumbnail for %1.").arg(QFileInfo(m_currentCoverPath).fileName()));
        startCoverExtraction();
        return;
    }
    m_coverProcess.start(QStringLiteral("7z"),
                         {QStringLiteral("x"), QStringLiteral("-so"), QStringLiteral("-y"),
                          QStringLiteral("-bd"), m_currentCoverPath, m_coverEntry});
}

void Backend::finishCoverExtraction(int exitCode, QProcess::ExitStatus exitStatus) {
    const bool pdf = QFileInfo(m_currentCoverPath).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0;
    bool success = exitCode == 0 && exitStatus == QProcess::NormalExit;
    if (m_coverFile) {
        success = success && m_coverFile->commit();
        m_coverFile.reset();
    } else if (pdf) {
        success = success && QFileInfo::exists(m_coverOutputPath);
    }
    if (success) {
        m_coverPaths.insert(m_currentCoverPath, m_coverOutputPath);
        emit comicsChanged();
    } else {
        setStatus(tr("Could not create a cover thumbnail for %1.").arg(QFileInfo(m_currentCoverPath).fileName()));
    }
    startCoverExtraction();
}

void Backend::openComic(const QString &path) {
    if (!storageReady())
        return;
    if (m_clearingCache) {
        setStatus(tr("Wait for image cache cleanup to finish."));
        return;
    }
    if (!m_books.contains(path)) {
        setStatus(tr("That book is not in the library."));
        return;
    }
    if (m_scanning && m_scanPath == path) {
        m_pendingOpenPath = path;
        setStatus(tr("The book is still being indexed."));
        return;
    }
    const QString stamp = fileStamp(QFileInfo(path));
    if (stamp.isEmpty()) {
        setStatus(tr("Choose a readable CBZ, CBR, CB7, CBT, or PDF file."));
        return;
    }
    if (stamp != m_books.value(path).value(QStringLiteral("indexStamp")).toString()) {
        m_pages.remove(path);
        m_coverPaths.remove(path);
        m_coverAttempted.remove(path);
    }
    if (stamp != m_books.value(path).value(QStringLiteral("indexStamp")).toString()
        || (isArchive(path) && !m_pages.contains(path) && !loadPageIndex(path))) {
        m_pendingOpenPath = path;
        setStatus(tr("The book is still being indexed."));
        requestIndex(path, true);
        return;
    }
    QJsonObject book = m_books.value(path);
    const int pageCount = book.value(QStringLiteral("pageCount")).toInt();
    if (pageCount <= 0) {
        setStatus(tr("This book has no readable pages."));
        return;
    }
    m_currentPath = path;
    m_rightToLeft = book.value(QStringLiteral("manga")).toBool();
    emit readerSettingsChanged();
    m_pageCount = pageCount;
    m_currentPage = qBound(1, book.value(QStringLiteral("progress")).toInt(1), m_pageCount);
    book.insert(QStringLiteral("lastRead"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    m_books.insert(path, book);
    m_reading = true;
    prepareReaderForCurrentBook();
    emit readerChanged();
    emit comicsChanged();
    writeCurrentStatus();
    saveState();
    startPageLoading();
}

void Backend::openExternalComic(const QString &path) {
    if (!storageReady())
        return;
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    const bool supported = isArchive(path) || suffix == QStringLiteral("pdf");
    if (!info.exists() || !info.isFile() || !info.isReadable() || !supported) {
        setStatus(tr("Choose a readable CBZ, CBR, CB7, CBT, or PDF file."));
        return;
    }
    const QString canonical = info.canonicalFilePath();
    const QString folder = info.absoluteDir().canonicalPath();
    if (canonical.isEmpty() || folder.isEmpty()) {
        setStatus(tr("The selected comic file could not be resolved."));
        return;
    }
    const bool inLibrary = std::any_of(m_folderPaths.cbegin(), m_folderPaths.cend(),
                                       [&canonical](const QString &root) {
        const QString cleanRoot = QDir::cleanPath(root);
        return canonical == cleanRoot || canonical.startsWith(cleanRoot + QDir::separator());
    });
    if (!inLibrary) {
        m_pendingOpenPath = canonical;
        addLibraryFolder(folder);
        return;
    }
    if (!m_books.contains(canonical)) {
        m_pendingOpenPath = canonical;
        if (!m_scanning)
            scanLibrary();
        requestIndex(canonical, true);
        return;
    }
    openComic(canonical);
}

void Backend::resumeComic() {
    if (m_currentPath.isEmpty() || !m_books.contains(m_currentPath)) {
        setStatus(tr("There is no book in progress yet."));
        return;
    }
    openComic(m_currentPath);
}

void Backend::prepareReaderForCurrentBook() {
    m_pageCount = m_books.value(m_currentPath).value(QStringLiteral("pageCount")).toInt();
    m_currentPages = m_pages.value(m_currentPath);
}

void Backend::startPageLoading() {
    if (!m_reading)
        return;
    ++m_pageLoadGeneration;
    m_pageFile.reset();
    if (m_pageProcess.state() != QProcess::NotRunning)
        m_pageProcess.kill();
    m_pagePath.clear();
    m_secondPagePath.clear();
    m_pageQueue.clear();
    m_pageQueue.enqueue(m_currentPage - 1);
    if (m_doublePage && m_currentPage > 1 && m_currentPage < m_pageCount)
        m_pageQueue.enqueue(m_currentPage);
    emit readerChanged();
    startNextPage();
}

void Backend::startNextPage() {
    if (m_pageProcess.state() != QProcess::NotRunning)
        return;
    if (m_pageQueue.isEmpty()) {
        finishPageLoad();
        return;
    }
    const int index = m_pageQueue.dequeue();
    const bool pdf = QFileInfo(m_currentPath).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0;
    QString entry;
    if (!pdf) {
        const QStringList pages = m_pages.value(m_currentPath);
        if (index < 0 || index >= pages.size()) {
            setStatus(tr("The requested page is not available."));
            startNextPage();
            return;
        }
        entry = pages.at(index);
    } else {
        entry = QStringLiteral("page-%1.png").arg(index + 1);
    }
    m_pageEntry = entry;
    m_pageOutputPath = pageCachePath(m_currentPath, entry);
    if (QFileInfo::exists(m_pageOutputPath)) {
        if (index == m_currentPage - 1)
            m_pagePath = localUrl(m_pageOutputPath);
        else
            m_secondPagePath = localUrl(m_pageOutputPath);
        startNextPage();
        return;
    }
    if (pdf) {
        m_activePageGeneration = m_pageLoadGeneration;
        m_pdfOutputPrefix = m_pageOutputPath.left(m_pageOutputPath.size() - 4);
        m_pageProcess.start(QStringLiteral("pdftoppm"),
                            {QStringLiteral("-f"), QString::number(index + 1), QStringLiteral("-l"),
                             QString::number(index + 1), QStringLiteral("-singlefile"),
                             QStringLiteral("-scale-to"), QStringLiteral("5000"), QStringLiteral("-png"),
                             m_currentPath, m_pdfOutputPrefix});
        return;
    }
    m_pageFile = std::make_unique<QSaveFile>(m_pageOutputPath);
    m_pageBytes = 0;
    if (!PrivateStorage::open(*m_pageFile)) {
        m_pageFile.reset();
        setStatus(tr("Could not cache page %1.").arg(index + 1));
        startNextPage();
        return;
    }
    m_activePageGeneration = m_pageLoadGeneration;
    m_pageProcess.start(QStringLiteral("7z"),
                        {QStringLiteral("x"), QStringLiteral("-so"), QStringLiteral("-y"),
                         QStringLiteral("-bd"), m_currentPath, entry});
}

void Backend::finishPageExtraction(int exitCode, QProcess::ExitStatus exitStatus) {
    if (m_activePageGeneration != m_pageLoadGeneration) {
        m_pageFile.reset();
        if (m_reading)
            startNextPage();
        return;
    }
    const bool pdf = QFileInfo(m_currentPath).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0;
    bool success = exitCode == 0 && exitStatus == QProcess::NormalExit;
    if (m_pageFile) {
        success = success && m_pageFile->commit();
        m_pageFile.reset();
    } else if (pdf) {
        success = success && QFileInfo::exists(m_pageOutputPath);
    }
    if (!success) {
        setStatus(tr("Could not extract page %1 from %2.")
                      .arg(m_currentPage)
                      .arg(QFileInfo(m_currentPath).fileName()));
        startNextPage();
        return;
    }
    const int loadedIndex = pdf ? m_pageEntry.mid(5, m_pageEntry.size() - 9).toInt() - 1
                                : m_pages.value(m_currentPath).indexOf(m_pageEntry);
    if (loadedIndex == m_currentPage - 1)
        m_pagePath = localUrl(m_pageOutputPath);
    else
        m_secondPagePath = localUrl(m_pageOutputPath);
    startNextPage();
}

void Backend::finishPageLoad() {
    if (m_pagePath.isEmpty())
        setStatus(tr("The page could not be displayed."));
    emit readerChanged();
}

void Backend::nextPage() {
    if (!m_reading || m_currentPage >= m_pageCount)
        return;
    if (m_doublePage && m_currentPage > 1 && m_currentPage + 2 > m_pageCount)
        return;
    const int step = m_doublePage && m_currentPage > 1 ? 2 : 1;
    m_currentPage = qMin(m_currentPage + step, m_pageCount);
    saveProgress();
    startPageLoading();
}

void Backend::previousPage() {
    if (!m_reading || m_currentPage <= 1)
        return;
    const int step = m_doublePage && m_currentPage > 2 ? 2 : 1;
    m_currentPage = qMax(1, m_currentPage - step);
    saveProgress();
    startPageLoading();
}

void Backend::openPage(int page) {
    if (!m_reading || page < 1 || page > m_pageCount)
        return;
    m_currentPage = page;
    saveProgress();
    startPageLoading();
}

void Backend::toggleBookmark() {
    if (!m_reading)
        return;
    QJsonObject book = m_books.value(m_currentPath);
    const QJsonArray bookmarks = book.value(QStringLiteral("bookmarks")).toArray();
    QJsonArray updated;
    bool removed = false;
    for (const QJsonValue &value : bookmarks) {
        if (value.toInt() == m_currentPage) {
            removed = true;
            continue;
        }
        updated.append(value);
    }
    if (!removed)
        updated.append(m_currentPage);
    book.insert(QStringLiteral("bookmarks"), updated);
    m_books.insert(m_currentPath, book);
    emit readerChanged();
    emit comicsChanged();
    saveState();
}

void Backend::saveProgress() {
    if (!m_books.contains(m_currentPath))
        return;
    QJsonObject book = m_books.value(m_currentPath);
    book.insert(QStringLiteral("progress"), m_currentPage);
    book.insert(QStringLiteral("lastRead"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    m_books.insert(m_currentPath, book);
    emit readerChanged();
    emit comicsChanged();
    saveState();
    writeCurrentStatus();
}

void Backend::closeReader() {
    if (!m_reading)
        return;
    m_reading = false;
    ++m_pageLoadGeneration;
    m_pageFile.reset();
    if (m_pageProcess.state() != QProcess::NotRunning)
        m_pageProcess.kill();
    m_pageQueue.clear();
    m_pagePath.clear();
    m_secondPagePath.clear();
    emit readerChanged();
}

void Backend::reportError(const QString &message) {
    setStatus(message);
}
