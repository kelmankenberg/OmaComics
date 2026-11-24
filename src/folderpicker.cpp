// The portal request/response flow follows Omacut (MIT, David Heinemeier Hansson).
#include "folderpicker.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QUrl>

FolderPicker::FolderPicker(QObject *parent) : QObject(parent) {}

bool FolderPicker::connectToRequestPath(const QString &path) {
    m_pendingPath = path;
    return QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"), path,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
        this, SLOT(handleResponse(uint,QVariantMap)));
}

void FolderPicker::clearPending() {
    if (!m_pendingPath.isEmpty())
        QDBusConnection::sessionBus().disconnect(
            QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
            QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
            this, SLOT(handleResponse(uint,QVariantMap)));
    m_pendingPath.clear();
    m_pendingToken.clear();
}

void FolderPicker::openFolder() {
    open(Selection::LibraryFolder);
}

void FolderPicker::openBackupFolder(const QString &directory) {
    open(Selection::BackupFolder, directory);
}

void FolderPicker::openRestoreFile(const QString &directory) {
    open(Selection::RestoreFile, directory);
}

void FolderPicker::open(Selection selection, const QString &directory) {
    clearPending();
    m_selection = selection;
    const QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface portal(QStringLiteral("org.freedesktop.portal.Desktop"),
                          QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.FileChooser"), bus);
    if (!portal.isValid()) {
        emit failed(tr("The XDG desktop portal file chooser is not available."));
        return;
    }

    const QString token = QStringLiteral("omacomics_%1").arg(QRandomGenerator::global()->generate());
    QVariantMap options{
        {QStringLiteral("accept_label"), selection == Selection::RestoreFile ? tr("Choose backup") : tr("Choose folder")},
        {QStringLiteral("modal"), true},
        {QStringLiteral("multiple"), false},
        {QStringLiteral("directory"), selection != Selection::RestoreFile},
        {QStringLiteral("handle_token"), token},
    };
    const QString home = QDir::homePath();
    const QString backupDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/backups");
    const QString start = !directory.isEmpty() && QDir(directory).exists() ? directory
        : selection == Selection::RestoreFile && QDir(backupDirectory).exists()
            ? backupDirectory : (QDir(home).exists() ? home : QStringLiteral("/"));
    const QByteArray folder = start.toUtf8() + '\0';
    options.insert(QStringLiteral("current_folder"), folder);

    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString expectedPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    m_pendingToken = token;
    if (!connectToRequestPath(expectedPath)) {
        clearPending();
        emit failed(tr("Could not listen for the folder chooser response."));
        return;
    }

    const QString title = selection == Selection::RestoreFile ? tr("Choose a library backup")
        : selection == Selection::BackupFolder ? tr("Choose a backup folder") : tr("Choose a comic library folder");
    auto *watcher = new QDBusPendingCallWatcher(
        portal.asyncCall(QStringLiteral("OpenFile"), QString(), title, options), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, token] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_pendingToken != token)
            return;
        if (reply.isError()) {
            const QString message = reply.error().message();
            clearPending();
            emit failed(tr("The folder chooser failed: %1").arg(message));
            return;
        }
        const QString actualPath = reply.value().path();
        if (actualPath != m_pendingPath) {
            QDBusConnection::sessionBus().disconnect(
                QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                this, SLOT(handleResponse(uint,QVariantMap)));
            if (!connectToRequestPath(actualPath)) {
                clearPending();
                emit failed(tr("Could not listen for the folder chooser response."));
            }
        }
    });
}

void FolderPicker::handleResponse(uint response, const QVariantMap &results) {
    clearPending();
    if (response != 0)
        return;
    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    if (uris.isEmpty()) {
        emit failed(tr("The file chooser returned no selection."));
        return;
    }
    const QUrl url(uris.first());
    if (!url.isLocalFile()) {
        emit failed(tr("Choose a file or folder on this device."));
        return;
    }
    if (m_selection == Selection::BackupFolder)
        emit backupFolderSelected(url.toLocalFile());
    else if (m_selection == Selection::RestoreFile)
        emit restoreFileSelected(url.toLocalFile());
    else
        emit folderSelected(url.toLocalFile());
}
