#pragma once

#include <QSaveFile>
#include <QString>

namespace PrivateStorage {
bool directory(const QString &path);
bool open(QSaveFile &file);
bool protectFile(const QString &path);
}
