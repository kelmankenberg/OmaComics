#pragma once

#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QQueue>
#include <QSaveFile>
#include <QSet>
#include <QVariantList>
#include <QXmlStreamReader>
#include <QObject>
#include <QUrl>
#include <memory>

#include "comiclistmodel.h"
#include "comictreemodel.h"
#include "comicvine.h"
#include "credentialstore.h"

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList comics READ comics NOTIFY comicsChanged)
    Q_PROPERTY(QVariantList visibleComics READ visibleComics NOTIFY comicsChanged)
    Q_PROPERTY(QAbstractItemModel *visibleComicModel READ visibleComicModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *comicTreeModel READ comicTreeModel CONSTANT)
    Q_PROPERTY(ComicVine *scraper READ scraper CONSTANT)
    Q_PROPERTY(QStringList selectedComics READ selectedComics NOTIFY selectionChanged)
    Q_PROPERTY(bool treeView READ treeView NOTIFY readerSettingsChanged)
    Q_PROPERTY(QStringList searchTerms READ searchTerms NOTIFY searchTermsChanged)
    Q_PROPERTY(QVariantList libraryFolders READ libraryFolders NOTIFY libraryFoldersChanged)
    Q_PROPERTY(QVariantList collections READ collections NOTIFY comicsChanged)
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY readerChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY readerChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY readerChanged)
    Q_PROPERTY(QString pagePath READ pagePath NOTIFY readerChanged)
    Q_PROPERTY(QString secondPagePath READ secondPagePath NOTIFY readerChanged)
    Q_PROPERTY(int currentPage READ currentPage NOTIFY readerChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY readerChanged)
    Q_PROPERTY(int thumbnailSize READ thumbnailSize NOTIFY thumbnailSizeChanged)
    Q_PROPERTY(bool reading READ reading NOTIFY readerChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool doublePage READ doublePage NOTIFY readerSettingsChanged)
    Q_PROPERTY(bool rightToLeft READ rightToLeft NOTIFY readerSettingsChanged)
    Q_PROPERTY(bool arrowNavigation READ arrowNavigation NOTIFY readerSettingsChanged)
    Q_PROPERTY(bool wheelNavigation READ wheelNavigation NOTIFY readerSettingsChanged)
    Q_PROPERTY(bool mouseNavigation READ mouseNavigation NOTIFY readerSettingsChanged)
    Q_PROPERTY(bool currentBookmarked READ currentBookmarked NOTIFY readerChanged)
    Q_PROPERTY(QString backupDirectory READ backupDirectory NOTIFY backupDirectoryChanged)
    Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY preferencesChanged)
    Q_PROPERTY(CredentialStore *credentials READ credentials CONSTANT)
    Q_PROPERTY(qint64 cacheBytes READ cacheBytes NOTIFY cacheChanged)
    Q_PROPERTY(bool cacheBusy READ cacheBusy NOTIFY cacheChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    Backend(const QString &dataDirectory, const QString &cacheDirectory, QObject *parent = nullptr,
            QNetworkAccessManager *network = nullptr, const QString &credentialTool = {});

    QVariantList comics() const;
    QVariantList visibleComics() const;
    QAbstractItemModel *visibleComicModel() { return &m_visibleComicModel; }
    QAbstractItemModel *comicTreeModel() { return &m_comicTreeModel; }
    ComicVine *scraper() { return &m_scraper; }
    QStringList selectedComics() const { return m_selectedComics; }
    bool treeView() const { return m_treeView; }
    QStringList searchTerms() const { return m_searchTerms; }
    QVariantList libraryFolders() const;
    QVariantList collections() const;
    QVariantList bookmarks() const;
    QString status() const { return m_status; }
    QString currentTitle() const;
    QString currentPath() const { return m_currentPath; }
    QString pagePath() const { return m_pagePath; }
    QString secondPagePath() const { return m_secondPagePath; }
    int currentPage() const { return m_currentPage; }
    int pageCount() const { return m_pageCount; }
    int thumbnailSize() const { return m_thumbnailSize; }
    bool reading() const { return m_reading; }
    bool scanning() const { return m_scanning; }
    bool doublePage() const { return m_doublePage; }
    bool rightToLeft() const { return m_rightToLeft; }
    bool arrowNavigation() const { return m_arrowNavigation; }
    bool wheelNavigation() const { return m_wheelNavigation; }
    bool mouseNavigation() const { return m_mouseNavigation; }
    bool currentBookmarked() const;
    QString backupDirectory() const { return m_backupDirectory; }
    QVariantMap preferences() const { return m_preferences.toVariantMap(); }
    CredentialStore *credentials() { return &m_credentials; }
    qint64 cacheBytes() const { return m_cacheBytes; }
    bool cacheBusy() const { return m_cacheActive; }
    bool backupCleanupBusy() const { return m_backupCleanupActive; }
    Q_INVOKABLE bool setPreference(const QString &name, const QVariant &value);
    Q_INVOKABLE void applyStartupBehavior();
    Q_INVOKABLE void loadComicVineKey();
    Q_INVOKABLE void setComicVineKey(const QString &key);
    Q_INVOKABLE void forgetComicVineKey();
    Q_INVOKABLE void refreshCacheSize();
    Q_INVOKABLE void clearImageCache();
    Q_INVOKABLE void setSelectedManga(bool enabled);

    Q_INVOKABLE void addLibraryFolder(const QString &path);
    Q_INVOKABLE void removeLibraryFolder(const QString &path);
    Q_INVOKABLE void removeFromRecent(const QString &path);
    Q_INVOKABLE void backupLibrary(const QString &directory = {});
    Q_INVOKABLE bool setBackupDirectory(const QString &directory);
    Q_INVOKABLE void openFeedback(bool bugReport);
    Q_INVOKABLE void restoreLibrary(const QString &path);
    Q_INVOKABLE void scanLibrary();
    Q_INVOKABLE void setSearchText(const QString &text);
    Q_INVOKABLE void commitSearchTerm();
    Q_INVOKABLE void removeSearchTerm(int index);
    Q_INVOKABLE void setCollectionFilter(const QString &collection);
    Q_INVOKABLE void setThumbnailSize(int size);
    Q_INVOKABLE void setDoublePage(bool enabled);
    Q_INVOKABLE void setRightToLeft(bool enabled);
    Q_INVOKABLE void setArrowNavigation(bool enabled);
    Q_INVOKABLE void setWheelNavigation(bool enabled);
    Q_INVOKABLE void setMouseNavigation(bool enabled);
    Q_INVOKABLE void setCollection(const QString &path, const QString &collection);
    Q_INVOKABLE void ensureCover(const QString &path);
    Q_INVOKABLE void openComic(const QString &path);
    Q_INVOKABLE void openExternalComic(const QString &path);
    Q_INVOKABLE void resumeComic();
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    Q_INVOKABLE void openPage(int page);
    Q_INVOKABLE void toggleBookmark();
    Q_INVOKABLE void closeReader();
    Q_INVOKABLE void reportError(const QString &message);
    Q_INVOKABLE void setTreeView(bool tree);
    Q_INVOKABLE void selectComic(const QString &path, bool selected);
    Q_INVOKABLE void selectFolder(const QString &path, bool selected);
    Q_INVOKABLE void selectAllVisible();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE QString scrapeQuery(const QString &path) const;
    Q_INVOKABLE QVariantList missingMetadata(const QString &path) const;
    Q_INVOKABLE bool applyScrapedMetadata(const QString &path);

signals:
    void comicsChanged();
    void searchTermsChanged();
    void libraryFoldersChanged();
    void statusChanged();
    void readerChanged();
    void thumbnailSizeChanged();
    void scanningChanged();
    void readerSettingsChanged();
    void selectionChanged();
    void backupDirectoryChanged();
    void preferencesChanged();
    void cacheChanged();
    void previewRequested(const QString &path);

private:
    enum class ScanStage { Idle, ArchiveList, ArchiveMetadata, PdfInfo };

    bool storageReady();
    void loadState();
    void loadSettings();
    bool savePreferences(const QJsonObject &preferences);
    void applyPreferences();
    void pruneBackups();
    void startCacheWork(bool clear);
    void setMangaForPaths(const QStringList &paths, bool enabled);
    void saveState();
    QByteArray libraryData() const;
    bool writeLibraryFile(const QString &path, const QByteArray &data);
    bool safetyBackup();
    bool dailyBackup();
    bool prepareBackupDirectory();
    void writeCurrentStatus();
    void setStatus(const QString &status);
    void setScanning(bool scanning);
    void updateBook(const QString &path, const QJsonObject &values);
    QVariantMap bookMap(const QString &path) const;
    QString bookTitle(const QString &path) const;
    QString pageCachePath(const QString &path, const QString &entry) const;
    QString coverCachePath(const QString &path, const QString &entry) const;
    QString pageIndexPath(const QString &path, const QString &stamp) const;
    bool loadPageIndex(const QString &path);
    bool savePageIndex(const QString &path, const QString &stamp);
    void requestIndex(const QString &path, bool priority);
    void finishIndexedFile(bool success);
    bool isArchive(const QString &path) const;
    bool isImage(const QString &path) const;

    void finishFolderSearch();
    void scanNextFile();
    void processScanOutput();
    void finishScanProcess(int exitCode, QProcess::ExitStatus exitStatus);
    void parseArchiveListing(const QByteArray &output);
    bool parseComicInfo(const QByteArray &xml);

    void startCoverExtraction();
    void finishCoverExtraction(int exitCode, QProcess::ExitStatus exitStatus);
    void startPageLoading();
    void startNextPage();
    void finishPageExtraction(int exitCode, QProcess::ExitStatus exitStatus);
    void finishPageLoad();
    void synchronizeVisibleComics();
    void saveProgress();
    void prepareReaderForCurrentBook();

    QStringList m_folderPaths;
    ComicListModel m_visibleComicModel;
    ComicTreeModel m_comicTreeModel;
    ComicVine m_scraper;
    CredentialStore m_credentials;
    QJsonObject m_preferences;
    bool m_preferencesLoaded = false;
    bool m_storageReady = false;
    QString m_storageError;
    QString m_lastPruned;
    struct CacheResult { qint64 bytes = 0; QString error; };
    QFutureWatcher<CacheResult> m_cacheWork;
    std::unique_ptr<QEventLoopLocker> m_cacheLocker;
    std::unique_ptr<QEventLoopLocker> m_scanLocker;
    QFutureWatcher<QString> m_backupCleanup;
    std::unique_ptr<QEventLoopLocker> m_backupLocker;
    bool m_clearingCache = false;
    bool m_cacheActive = false;
    bool m_backupCleanupActive = false;
    qint64 m_cacheBytes = 0;
    QStringList m_selectedComics;
    QHash<QString, QJsonObject> m_books;
    QStringList m_bookOrder;
    QHash<QString, QStringList> m_pages;
    QHash<QString, QString> m_coverPaths;
    QSet<QString> m_coverAttempted;
    QSet<QString> m_scheduledCoverIndexes;
    QSet<QString> m_pendingCovers;
    QQueue<QString> m_coverQueue;

    QString m_dataDirectory;
    QString m_cacheDirectory;
    QString m_libraryFile;
    QString m_backupDirectory;
    QString m_currentStatusFile;
    QString m_currentPath;
    QString m_pagePath;
    QString m_secondPagePath;
    QString m_searchText;
    QStringList m_searchTerms;
    QString m_collectionFilter;
    QString m_status = QStringLiteral("Add a library folder to get started.");
    QString m_scanPath;
    QString m_metadataEntry;
    QString m_pendingOpenPath;
    QString m_pageOutputPath;
    QString m_pageEntry;
    QString m_coverOutputPath;
    QString m_coverEntry;
    QString m_currentCoverPath;
    QString m_pdfOutputPrefix;
    QByteArray m_scanOutput;
    std::unique_ptr<QSaveFile> m_pageFile;
    std::unique_ptr<QSaveFile> m_coverFile;
    QStringList m_scanQueue;
    QQueue<int> m_pageQueue;
    QFutureWatcher<QList<QFileInfo>> m_folderSearch;
    QProcess m_scanProcess;
    QProcess m_coverProcess;
    QProcess m_pageProcess;
    ScanStage m_scanStage = ScanStage::Idle;
    QStringList m_currentPages;
    QStringList m_deferredIndexes;
    QString m_scanStamp;
    QString m_scanError;
    QJsonObject m_scannedValues;
    QDateTime m_librarySavedAt;
    qsizetype m_pageBytes = 0;
    qsizetype m_coverBytes = 0;
    int m_currentPage = 0;
    int m_pageCount = 0;
    int m_pageLoadGeneration = 0;
    int m_activePageGeneration = 0;
    int m_thumbnailSize = 88;
    bool m_scanning = false;
    bool m_discovering = false;
    bool m_synchronizingComics = false;
    bool m_comicsSyncPending = false;
    bool m_reading = false;
    bool m_treeView = false;
    bool m_doublePage = false;
    bool m_rightToLeft = false;
    bool m_arrowNavigation = true;
    bool m_wheelNavigation = true;
    bool m_mouseNavigation = true;
    bool m_scanOutputTooLarge = false;
    bool m_rescanAfterCurrent = false;
};
