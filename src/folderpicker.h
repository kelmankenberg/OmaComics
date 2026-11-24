#pragma once

#include <QObject>

class FolderPicker : public QObject {
    Q_OBJECT

public:
    explicit FolderPicker(QObject *parent = nullptr);
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void openBackupFolder(const QString &directory = {});
    Q_INVOKABLE void openRestoreFile(const QString &directory = {});

signals:
    void folderSelected(const QString &path);
    void backupFolderSelected(const QString &path);
    void restoreFileSelected(const QString &path);
    void failed(const QString &message);

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Selection { LibraryFolder, BackupFolder, RestoreFile };
    void open(Selection selection, const QString &directory = {});
    bool connectToRequestPath(const QString &path);
    void clearPending();

    QString m_pendingPath;
    QString m_pendingToken;
    Selection m_selection = Selection::LibraryFolder;
};
