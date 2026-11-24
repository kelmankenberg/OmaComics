#pragma once

#include <QEventLoopLocker>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <memory>

class CredentialStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
public:
    explicit CredentialStore(const QString &tool, QObject *parent = nullptr);
    bool available() const { return !m_tool.isEmpty(); }
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    void load();
    void store(const QString &key);
    void forget();
signals:
    void changed();
    void loaded(const QString &key);
private:
    enum class Action { Load, Store, Forget };
    void start(Action action, const QString &key = {});
    void finish(bool success);
    QString m_tool;
    QString m_message;
    QByteArray m_output;
    QProcess m_process;
    QTimer m_timeout;
    std::unique_ptr<QEventLoopLocker> m_locker;
    Action m_action = Action::Load;
    bool m_busy = false;
    bool m_errorOutput = false;
    bool m_failed = false;
};
