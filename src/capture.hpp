#pragma once

#include "frame.hpp"
#include <QDBusUnixFileDescriptor>
#include <QSocketNotifier>
#include <QObject>

class Capture : public QObject {
    Q_OBJECT
public:
    explicit Capture(QObject *parent = nullptr);
    ~Capture() override;
    void start(QDBusUnixFileDescriptor fd, uint32_t node, const QString &serial,
               const std::vector<DmaFormat> &formats);
    void stop();
    void retryShm();
    Frame takeFrame();
    void release(pw_buffer *buffer);

signals:
    void frameReady();
    void aboutToStop();
    void bufferRemoved(pw_buffer *buffer);
    void status(QString message);
    void failed(QString message);

private:
    void connectStream();
    void process();
    void paramChanged(uint32_t id, const spa_pod *param);
    void reportError(const QString &message);
    static const pw_stream_events events;
    static const pw_core_events coreEvents;
    pw_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook streamListener{};
    spa_hook coreListener{};
    QSocketNotifier *notifier = nullptr;
    spa_video_info_raw format{};
    Frame pending;
    QDBusUnixFileDescriptor remote;
    uint32_t target = 0;
    QString targetSerial;
    std::vector<DmaFormat> dmaFormats;
    bool stopping = false;
    bool errorReported = false;
};
