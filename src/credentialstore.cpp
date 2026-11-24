#include "credentialstore.h"
#include <QRegularExpression>

CredentialStore::CredentialStore(const QString &tool, QObject *parent) : QObject(parent), m_tool(tool) {
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_failed = true;
        m_process.kill();
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_output.append(m_process.readAllStandardOutput());
        if (m_output.size() > 256) {
            m_output.clear();
            m_failed = true;
            m_process.kill();
        }
    });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_errorOutput |= !m_process.readAllStandardError().isEmpty();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false);
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        const bool absent = m_action != Action::Store && code == 1 && !m_errorOutput && m_output.isEmpty();
        finish(!m_failed && status == QProcess::NormalExit && (code == 0 || absent));
    });
}

void CredentialStore::load() { start(Action::Load); }
void CredentialStore::store(const QString &key) { start(Action::Store, key); }
void CredentialStore::forget() { start(Action::Forget); }

void CredentialStore::start(Action action, const QString &key) {
    if (m_busy) {
        m_message = tr("Wait for credential storage to finish.");
        emit changed();
        return;
    }
    if (!available()) {
        m_message = tr("Desktop credential storage is unavailable. The API key remains session-only.");
        emit changed();
        return;
    }
    m_action = action;
    m_busy = true;
    m_failed = false;
    m_errorOutput = false;
    m_output.clear();
    m_message.clear();
    m_locker = std::make_unique<QEventLoopLocker>();
    emit changed();
    QStringList arguments;
    if (action == Action::Store)
        arguments = {QStringLiteral("store"), QStringLiteral("--label=OmaComics Comic Vine API key")};
    else
        arguments = {action == Action::Load ? QStringLiteral("lookup") : QStringLiteral("clear")};
    arguments.append({QStringLiteral("application"), QStringLiteral("omacomics"),
                      QStringLiteral("service"), QStringLiteral("comicvine")});
    m_process.start(m_tool, arguments);
    if (action == Action::Store)
        m_process.write(key.toUtf8());
    m_process.closeWriteChannel();
    m_timeout.start(30000);
}

void CredentialStore::finish(bool success) {
    m_timeout.stop();
    m_busy = false;
    const QString key = QString::fromUtf8(m_output).trimmed();
    if (m_action == Action::Load && !key.isEmpty()
        && !QRegularExpression(QStringLiteral("^[A-Za-z0-9]{16,128}$")).match(key).hasMatch())
        success = false;
    if (!success) {
        m_message = tr("Credential storage failed or was cancelled. No key was saved to application files.");
    } else if (m_action == Action::Load) {
        if (!m_output.trimmed().isEmpty())
            emit loaded(key);
        m_message = m_output.trimmed().isEmpty() ? tr("No saved API key.") : tr("API key loaded from desktop credential storage.");
    } else {
        m_message = m_action == Action::Store ? tr("API key saved in desktop credential storage.")
                                            : tr("Saved API key removed.");
    }
    m_output.fill('\0');
    m_output.clear();
    emit changed();
    m_locker.reset();
}
