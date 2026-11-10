#include "portal.hpp"
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QtTest>
#include <fcntl.h>
#include <unistd.h>

struct StreamInfo { uint node; QVariantMap properties; };
Q_DECLARE_METATYPE(StreamInfo)
using Streams = QList<StreamInfo>;
Q_DECLARE_METATYPE(Streams)
QDBusArgument &operator<<(QDBusArgument &arg, const StreamInfo &stream)
{
    arg.beginStructure(); arg << stream.node << stream.properties; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, StreamInfo &stream)
{
    arg.beginStructure(); arg >> stream.node >> stream.properties; arg.endStructure(); return arg;
}

class FakePortal : public QDBusVirtualObject {
public:
    uint responseCode = 0;
    uint sourceType = 2;
    bool omitStreams = false;
    bool delayed = false;
    int selectCount = 0, sessionCloseCount = 0, requestCloseCount = 0;
    bool optionsValid = true;
    QString session = "/org/freedesktop/portal/desktop/session/test/session";
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override
    {
        if (message.member() == "Close") {
            if (message.interface() == "org.freedesktop.portal.Session") ++sessionCloseCount;
            else ++requestCloseCount;
            bus.send(message.createReply());
            return true;
        }
        if (message.member() == "OpenPipeWireRemote") {
            const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
            QDBusUnixFileDescriptor descriptor(fd);
            close(fd);
            bus.send(message.createReply({QVariant::fromValue(descriptor)}));
            return true;
        }
        const auto options = qdbus_cast<QVariantMap>(message.arguments().last());
        QString sender = message.service().mid(1);
        sender.replace('.', '_');
        const QString handle = "/org/freedesktop/portal/desktop/request/" + sender + '/' + options.value("handle_token").toString();
        QVariantMap result;
        if (message.member() == "CreateSession") {
            result.insert("session_handle", session);
        } else if (message.member() == "SelectSources") {
            ++selectCount;
            optionsValid &= options.value("types").metaType().id() == QMetaType::UInt &&
                options.value("types").toUInt() == 2 && !options.value("multiple").toBool();
        } else if (message.member() == "Start" && !omitStreams) {
            result.insert("streams", QVariant::fromValue(Streams{{7,
                {{"source_type", sourceType}, {"pipewire-serial", qulonglong(99)}}}}));
        }
        auto respond = [bus, handle, result, code = message.member() == "Start" ? responseCode : 0u] {
            auto signal = QDBusMessage::createSignal(handle, "org.freedesktop.portal.Request", "Response");
            signal.setArguments({code, result});
            bus.send(signal);
        };
        // Exercise the signal-before-method-reply race.
        if (delayed) QTimer::singleShot(100, this, respond);
        else respond();
        bus.send(message.createReply({QVariant::fromValue(QDBusObjectPath(handle))}));
        return true;
    }
};

class PortalTest : public QObject {
    Q_OBJECT
    FakePortal fake;
private slots:
    void initTestCase()
    {
        qDBusRegisterMetaType<StreamInfo>();
        qDBusRegisterMetaType<Streams>();
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerService("org.freedesktop.portal.Desktop"));
        QVERIFY(bus.registerVirtualObject("/org/freedesktop/portal/desktop", &fake,
                                          QDBusConnection::SubPath));
    }
    void init()
    {
        fake.responseCode = 0;
        fake.sourceType = 2;
        fake.omitStreams = false;
        fake.delayed = false;
        fake.selectCount = fake.sessionCloseCount = fake.requestCloseCount = 0;
        fake.optionsValid = true;
    }
    void windowSession()
    {
        Portal portal;
        QSignalSpy ready(&portal, &Portal::ready);
        portal.start();
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(ready.at(0).at(0).value<QDBusUnixFileDescriptor>().isValid());
        QCOMPARE(ready.at(0).at(1).toUInt(), 7u);
        QCOMPARE(ready.at(0).at(2).toString(), QString("99"));
        QCOMPARE(fake.selectCount, 1);
        QVERIFY(fake.optionsValid);
        portal.stop();
        QTRY_COMPARE(fake.sessionCloseCount, 1);
    }
    void cancelAndRestart()
    {
        Portal portal;
        QSignalSpy ready(&portal, &Portal::ready);
        QSignalSpy ended(&portal, &Portal::ended);
        fake.responseCode = 1;
        portal.start();
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ready.count(), 0);
        QTRY_COMPARE(fake.sessionCloseCount, 1);
        fake.responseCode = 0;
        portal.start();
        QTRY_COMPARE(ready.count(), 1);
    }
    void monitorRejected()
    {
        Portal portal;
        QSignalSpy ready(&portal, &Portal::ready);
        QSignalSpy ended(&portal, &Portal::ended);
        fake.sourceType = 1;
        portal.start();
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ready.count(), 0);
    }
    void malformedStreams()
    {
        Portal portal;
        QSignalSpy ready(&portal, &Portal::ready);
        QSignalSpy ended(&portal, &Portal::ended);
        fake.omitStreams = true;
        portal.start();
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ready.count(), 0);
    }
    void stopDuringRequest()
    {
        Portal portal;
        QSignalSpy ready(&portal, &Portal::ready);
        fake.delayed = true;
        portal.start();
        QTest::qWait(20);
        portal.stop();
        QTRY_COMPARE(fake.requestCloseCount, 1);
        QTest::qWait(150);
        QCOMPARE(ready.count(), 0);
        fake.delayed = false;
        portal.start();
        QTRY_COMPARE(ready.count(), 1);
    }
    void desktopClosesSession()
    {
        Portal portal;
        QSignalSpy ready(&portal, &Portal::ready);
        QSignalSpy ended(&portal, &Portal::ended);
        portal.start();
        QTRY_COMPARE(ready.count(), 1);
        auto signal = QDBusMessage::createSignal(fake.session, "org.freedesktop.portal.Session", "Closed");
        signal.setArguments({QVariantMap()});
        QDBusConnection::sessionBus().send(signal);
        QTRY_COMPARE(ended.count(), 1);
    }
};

QTEST_GUILESS_MAIN(PortalTest)
#include "test_portal.moc"
