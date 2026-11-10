#pragma once

#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QVariantMap>
#include <QObject>
#include <QTimer>

class Portal : public QObject {
    Q_OBJECT
public:
    explicit Portal(QObject *parent = nullptr);
    ~Portal() override;
    void start();
    void stop();

signals:
    void selecting();
    void ready(QDBusUnixFileDescriptor fd, uint32_t node, QString serial);
    void status(QString message);
    void ended();

private slots:
    void response(uint code, const QVariantMap &results);
    void sessionClosed(const QVariantMap &details);

private:
    enum class Step { Idle, Create, Select, Start, Open, Running };
    void request(const QString &method, QVariantList args, QVariantMap options);
    void openRemote(const QVariantMap &results);
    void fail(const QString &message);
    void clearRequest();
    QDBusConnection bus = QDBusConnection::sessionBus();
    Step step = Step::Idle;
    QString session;
    QString requestPath;
    QTimer timeout;
    uint64_t generation = 0;
};
