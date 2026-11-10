#include "portal.hpp"

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QUuid>

namespace {
const QString service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString path = QStringLiteral("/org/freedesktop/portal/desktop");
const QString screenCast = QStringLiteral("org.freedesktop.portal.ScreenCast");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");
const QString sessionInterface = QStringLiteral("org.freedesktop.portal.Session");

QString token()
{
    return QStringLiteral("rotate_") + QUuid::createUuid().toString(QUuid::Id128);
}
}

Portal::Portal(QObject *parent) : QObject(parent)
{
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, this, [this] {
        fail(tr("系统窗口选择器响应超时，请检查桌面的 ScreenCast portal。"));
    });
}

Portal::~Portal() { stop(); }

void Portal::start()
{
    stop();
    if (!bus.isConnected()) {
        fail(tr("无法连接桌面 D-Bus 会话。"));
        return;
    }
    step = Step::Create;
    emit selecting();
    emit status(tr("正在打开系统窗口选择器…"));
    request(QStringLiteral("CreateSession"), {}, {{"session_handle_token", token()}});
}

void Portal::request(const QString &method, QVariantList args, QVariantMap options)
{
    clearRequest();
    const QString handle = token();
    options.insert("handle_token", handle);
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    requestPath = "/org/freedesktop/portal/desktop/request/" + sender + '/' + handle;
    // Subscribe before the method call: a fast portal can respond before its reply.
    if (!bus.connect(service, requestPath, requestInterface, "Response", this,
                     SLOT(response(uint,QVariantMap)))) {
        fail(tr("无法订阅系统窗口选择器。"));
        return;
    }
    args.append(options);
    auto message = QDBusMessage::createMethodCall(service, path, screenCast, method);
    message.setArguments(args);
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    const auto current = generation;
    const QString expected = requestPath;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, current, expected, watcher] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (generation != current || requestPath != expected)
            return;
        if (reply.isError()) {
            fail(tr("窗口采集请求失败：%1").arg(reply.error().message()));
        } else if (reply.value().path() != requestPath) {
            bus.disconnect(service, requestPath, requestInterface, "Response", this,
                           SLOT(response(uint,QVariantMap)));
            requestPath = reply.value().path();
            if (!bus.connect(service, requestPath, requestInterface, "Response", this,
                             SLOT(response(uint,QVariantMap))))
                fail(tr("无法订阅窗口采集请求。"));
        }
    });
    timeout.start(120000);
}

void Portal::response(uint code, const QVariantMap &results)
{
    clearRequest();
    if (code != 0) {
        fail(code == 1 ? tr("已取消窗口选择。") : tr("桌面拒绝了窗口采集请求。"));
        return;
    }
    switch (step) {
    case Step::Create:
        session = results.value("session_handle").toString();
        if (!session.startsWith("/org/freedesktop/portal/desktop/session/")) {
            fail(tr("桌面返回了无效的采集会话。"));
            return;
        }
        bus.connect(service, session, sessionInterface, "Closed", this,
                    SLOT(sessionClosed(QVariantMap)));
        step = Step::Select;
        // WINDOW only. Never silently fall back to sharing the entire monitor.
        request("SelectSources", {QVariant::fromValue(QDBusObjectPath(session))},
                {{"types", uint(2)}, {"multiple", false}});
        break;
    case Step::Select:
        step = Step::Start;
        request("Start", {QVariant::fromValue(QDBusObjectPath(session)), QString()}, {});
        break;
    case Step::Start:
        openRemote(results);
        break;
    default:
        break;
    }
}

void Portal::openRemote(const QVariantMap &results)
{
    uint32_t node = 0;
    QVariantMap properties;
    const auto streams = qvariant_cast<QDBusArgument>(results.value("streams"));
    if (streams.currentSignature() != "a(ua{sv})") {
        fail(tr("桌面没有返回有效的窗口视频流。"));
        return;
    }
    streams.beginArray();
    if (!streams.atEnd()) {
        streams.beginStructure();
        streams >> node >> properties;
        streams.endStructure();
    }
    const bool multiple = !streams.atEnd();
    streams.endArray();
    if (!node || multiple || (properties.contains("source_type") &&
                              properties.value("source_type").toUInt() != 2)) {
        fail(tr("请只选择一个窗口；当前桌面可能不支持窗口采集。"));
        return;
    }
    const QString serial = properties.contains("pipewire-serial")
        ? QString::number(properties.value("pipewire-serial").toULongLong()) : QString();
    step = Step::Open;
    auto message = QDBusMessage::createMethodCall(service, path, screenCast, "OpenPipeWireRemote");
    message.setArguments({QVariant::fromValue(QDBusObjectPath(session)), QVariantMap()});
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    const auto current = generation;
    timeout.start(10000);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, current, node, serial] {
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
        watcher->deleteLater();
        if (current != generation || step != Step::Open)
            return;
        timeout.stop();
        if (reply.isError() || !reply.value().isValid()) {
            fail(tr("无法打开 PipeWire 视频流：%1").arg(reply.error().message()));
            return;
        }
        step = Step::Running;
        emit ready(reply.value(), node, serial);
    });
}

void Portal::clearRequest()
{
    timeout.stop();
    if (!requestPath.isEmpty()) {
        bus.disconnect(service, requestPath, requestInterface, "Response", this,
                       SLOT(response(uint,QVariantMap)));
        requestPath.clear();
    }
}

void Portal::stop()
{
    ++generation;
    if (!requestPath.isEmpty())
        bus.asyncCall(QDBusMessage::createMethodCall(service, requestPath, requestInterface, "Close"));
    clearRequest();
    if (!session.isEmpty()) {
        bus.disconnect(service, session, sessionInterface, "Closed", this,
                       SLOT(sessionClosed(QVariantMap)));
        bus.asyncCall(QDBusMessage::createMethodCall(service, session, sessionInterface, "Close"));
        session.clear();
    }
    const bool wasActive = step != Step::Idle;
    step = Step::Idle;
    if (wasActive)
        emit ended();
}

void Portal::fail(const QString &message)
{
    stop();
    emit status(message);
}

void Portal::sessionClosed(const QVariantMap &)
{
    session.clear();
    fail(tr("窗口采集已由桌面结束。"));
}
