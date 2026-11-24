#include "privatestorage.h"

#include <QDir>
#include <QFileInfo>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool createDirectory(const QString &path) {
    const QByteArray name = QFile::encodeName(path);
    if (::mkdir(name.constData(), 0700) == 0 || errno == EEXIST)
        return true;
    if (errno != ENOENT)
        return false;
    const QString parent = QFileInfo(path).absolutePath();
    return parent != path && createDirectory(parent) && ::mkdir(name.constData(), 0700) == 0;
}

bool protect(int fd, mode_t mode, bool directory) {
    if (fd < 0)
        return false;
    struct stat info {};
    const bool success = ::fstat(fd, &info) == 0 && info.st_uid == ::geteuid()
        && (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_nlink == 1)
        && ::fchmod(fd, mode) == 0;
    ::close(fd);
    return success;
}

} // namespace

bool PrivateStorage::directory(const QString &path) {
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (clean == QDir::rootPath() || clean == QDir::homePath() || !createDirectory(clean))
        return false;
    return protect(::open(QFile::encodeName(clean).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                   0700, true);
}

bool PrivateStorage::open(QSaveFile &file) {
    file.setDirectWriteFallback(false);
    if (QFileInfo(file.fileName()).isSymLink() || !file.open(QIODevice::WriteOnly))
        return false;
    // QSaveFile otherwise preserves or applies umask-derived final permissions.
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
}

bool PrivateStorage::protectFile(const QString &path) {
    return protect(::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC),
                   0600, false);
}
