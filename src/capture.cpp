#include "capture.hpp"

#include <QTimer>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

const pw_stream_events Capture::events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = [](void *data, pw_stream_state, pw_stream_state state, const char *error) {
        auto *self = static_cast<Capture *>(data);
        if (state == PW_STREAM_STATE_ERROR)
            self->reportError(QObject::tr("PipeWire 采集失败：%1").arg(QString::fromUtf8(error)));
        else if (state == PW_STREAM_STATE_UNCONNECTED && !self->stopping)
            self->reportError(QObject::tr("目标窗口的视频流已断开。"));
    };
    e.param_changed = [](void *data, uint32_t id, const spa_pod *param) {
        static_cast<Capture *>(data)->paramChanged(id, param);
    };
    e.remove_buffer = [](void *data, pw_buffer *buffer) {
        auto *self = static_cast<Capture *>(data);
        if (self->pending.buffer == buffer)
            self->pending = {};
        emit self->bufferRemoved(buffer);
    };
    e.process = [](void *data) { static_cast<Capture *>(data)->process(); };
    return e;
}();

const pw_core_events Capture::coreEvents = [] {
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.error = [](void *data, uint32_t id, int, int result, const char *message) {
        if (id == PW_ID_CORE || result == -EPIPE)
            static_cast<Capture *>(data)->reportError(
                QObject::tr("PipeWire 连接错误：%1").arg(QString::fromUtf8(message)));
    };
    return e;
}();

Capture::Capture(QObject *parent) : QObject(parent) {}
Capture::~Capture() { stop(); }

void Capture::start(QDBusUnixFileDescriptor fd, uint32_t node, const QString &serial,
                    const std::vector<DmaFormat> &formats)
{
    stop();
    stopping = false;
    errorReported = false;
    remote = fd;
    target = node;
    targetSerial = serial;
    dmaFormats = formats;
    loop = pw_loop_new(nullptr);
    if (!loop) {
        reportError(tr("无法创建 PipeWire 事件循环。"));
        return;
    }
    pw_loop_enter(loop);
    context = pw_context_new(loop, nullptr, 0);
    if (!context) {
        reportError(tr("无法创建 PipeWire 上下文。"));
        return;
    }
    const int duplicated = fd.isValid() ? fcntl(fd.fileDescriptor(), F_DUPFD_CLOEXEC, 0) : -1;
    if (duplicated < 0) {
        reportError(tr("无法复制 PipeWire 授权连接。"));
        return;
    }
    core = pw_context_connect_fd(context, duplicated, nullptr, 0);
    if (!core) {
        reportError(tr("无法连接 PipeWire：%1").arg(QString::fromLocal8Bit(strerror(errno))));
        return;
    }
    pw_core_add_listener(core, &coreListener, &coreEvents, this);
    notifier = new QSocketNotifier(pw_loop_get_fd(loop), QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this] {
        if (loop && pw_loop_iterate(loop, 0) < 0)
            reportError(tr("PipeWire 事件处理失败。"));
    });
    connectStream();
}

void Capture::connectStream()
{
    auto *properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_NODE_NAME, "wayland-rotate-window", nullptr);
    if (!targetSerial.isEmpty())
        pw_properties_set(properties, PW_KEY_TARGET_OBJECT, targetSerial.toUtf8().constData());
    stream = pw_stream_new(core, "Wayland Rotate Window", properties);
    if (!stream) {
        reportError(tr("无法创建窗口视频流。"));
        return;
    }
    pw_stream_add_listener(stream, &streamListener, &events, this);
    alignas(8) uint8_t storage[65536];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    std::vector<const spa_pod *> params;
    const spa_rectangle minSize{1, 1}, maxSize{16384, 16384}, defaultSize{1920, 1080};
    auto addFormat = [&](spa_video_format pixelFormat, const std::vector<uint64_t> &modifiers) {
        spa_pod_frame object;
        spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&builder,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(pixelFormat),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defaultSize, &minSize, &maxSize), 0);
        if (!modifiers.empty()) {
            spa_pod_frame choice;
            spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_push_choice(&builder, &choice, SPA_CHOICE_Enum, 0);
            spa_pod_builder_long(&builder, modifiers.front());
            for (uint64_t modifier : modifiers)
                spa_pod_builder_long(&builder, modifier);
            spa_pod_builder_pop(&builder, &choice);
        }
        params.push_back(static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &object)));
    };
    for (const auto &dma : dmaFormats)
        addFormat(dma.format, dma.modifiers);
    for (auto pixelFormat : {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx,
                            SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx})
        addFormat(pixelFormat, {});
    if (spa_pod_builder_corrupted(&builder)) {
        reportError(tr("GPU 视频格式列表超过协商容量。"));
        return;
    }
    // Old portals only return a node ID. Modern ones supply a stable object serial.
    const int result = pw_stream_connect(stream, PW_DIRECTION_INPUT,
        targetSerial.isEmpty() ? target : PW_ID_ANY,
        pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params.data(), params.size());
    if (result < 0)
        reportError(tr("无法连接目标窗口：%1").arg(QString::fromLocal8Bit(strerror(-result))));
}

void Capture::paramChanged(uint32_t id, const spa_pod *param)
{
    if (id != SPA_PARAM_Format || !param)
        return;
    spa_video_info_raw next{};
    if (spa_format_video_raw_parse(param, &next) < 0 || next.size.width == 0 ||
        next.size.height == 0 || next.size.width > 16384 || next.size.height > 16384) {
        reportError(tr("桌面返回了不支持的视频格式或尺寸。"));
        return;
    }
    if (next.format != SPA_VIDEO_FORMAT_BGRA && next.format != SPA_VIDEO_FORMAT_BGRx &&
        next.format != SPA_VIDEO_FORMAT_RGBA && next.format != SPA_VIDEO_FORMAT_RGBx) {
        reportError(tr("桌面返回了不支持的像素格式。"));
        return;
    }
    format = next;
    alignas(8) uint8_t storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const int types = format.flags & SPA_VIDEO_FLAG_MODIFIER ? 1 << SPA_DATA_DmaBuf
        : (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
    const spa_pod *params[] = {
        static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(3, 3, 8),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types))),
        static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))),
        static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region))))
    };
    const int result = pw_stream_update_params(stream, params, 3);
    if (result < 0) {
        reportError(tr("视频缓冲区协商失败。"));
        return;
    }
    emit status(tr("%1 × %2 · %3 · GPU 旋转")
        .arg(format.size.width).arg(format.size.height)
        .arg(format.flags & SPA_VIDEO_FLAG_MODIFIER ? tr("DMA-BUF") : tr("共享内存")));
}

void Capture::process()
{
    pw_buffer *latest = nullptr;
    while (auto *buffer = pw_stream_dequeue_buffer(stream)) {
        if (latest)
            pw_stream_queue_buffer(stream, latest);
        latest = buffer;
    }
    if (!latest)
        return;
    auto *buffer = latest->buffer;
    auto *header = static_cast<spa_meta_header *>(spa_buffer_find_meta_data(
        buffer, SPA_META_Header, sizeof(spa_meta_header)));
    if (!buffer->n_datas || !buffer->datas[0].chunk ||
        buffer->datas[0].chunk->flags & (SPA_CHUNK_FLAG_CORRUPTED | SPA_CHUNK_FLAG_EMPTY) ||
        (header && header->flags & (SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_GAP))) {
        release(latest);
        return;
    }
    auto crop = frameCrop(buffer, QSize(format.size.width, format.size.height));
    if (!crop || crop->isEmpty()) {
        release(latest);
        return;
    }
    release(pending.buffer);
    pending = {latest, format, *crop};
    emit frameReady();
}

Frame Capture::takeFrame()
{
    Frame result = pending;
    pending = {};
    return result;
}

void Capture::release(pw_buffer *buffer)
{
    if (buffer && stream)
        pw_stream_queue_buffer(stream, buffer);
}

void Capture::retryShm()
{
    if (stopping || !stream || !core)
        return;
    if (dmaFormats.empty()) {
        reportError(tr("共享内存视频流无法显示。"));
        return;
    }
    const auto fd = remote;
    const auto node = target;
    const auto serial = targetSerial;
    start(fd, node, serial, {});
    emit status(tr("GPU 导入失败，已切换共享内存采集…"));
}

void Capture::reportError(const QString &message)
{
    if (!stopping && !errorReported) {
        errorReported = true;
        emit failed(message);
    }
}

void Capture::stop()
{
    stopping = true;
    emit aboutToStop();
    release(pending.buffer);
    pending = {};
    delete notifier;
    notifier = nullptr;
    if (stream) {
        pw_stream_destroy(stream);
        stream = nullptr;
    }
    if (core) {
        spa_hook_remove(&coreListener);
        pw_core_disconnect(core);
        core = nullptr;
    }
    if (context) {
        pw_context_destroy(context);
        context = nullptr;
    }
    if (loop) {
        pw_loop_leave(loop);
        pw_loop_destroy(loop);
        loop = nullptr;
    }
    remote = {};
    format = {};
}
