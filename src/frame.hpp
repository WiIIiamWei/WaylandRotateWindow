#pragma once

#include <QPointF>
#include <QRect>
#include <QSize>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>
#include <optional>
#include <vector>

struct DmaFormat {
    spa_video_format format;
    uint32_t fourcc;
    std::vector<uint64_t> modifiers;
};

struct Frame {
    pw_buffer *buffer = nullptr;
    spa_video_info_raw format{};
    QRect crop;
};

inline QSize rotatedSize(QSize size, int rotation)
{
    return rotation % 180 ? size.transposed() : size;
}

// Rotate first, then flip in the displayed axes.
inline QPointF sourceUv(QPointF uv, int rotation, bool flipX, bool flipY)
{
    const double x = flipX ? 1 - uv.x() : uv.x();
    const double y = flipY ? 1 - uv.y() : uv.y();
    switch (rotation) {
    case 90: return {y, 1 - x};
    case 180: return {1 - x, 1 - y};
    case 270: return {1 - y, x};
    default: return {x, y};
    }
}

inline std::optional<QRect> frameCrop(const spa_buffer *buffer, QSize size)
{
    const QRect bounds(QPoint(0, 0), size);
    auto *crop = static_cast<spa_meta_region *>(spa_buffer_find_meta_data(
        buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)));
    if (!crop || !spa_meta_region_is_valid(crop))
        return bounds;
    const QRect region(crop->region.position.x, crop->region.position.y,
                       crop->region.size.width, crop->region.size.height);
    return bounds.contains(region) ? std::optional<QRect>(region) : std::nullopt;
}

inline bool validShm(const spa_data &data, QSize size)
{
    if (!data.data || !data.chunk || !data.maxsize || size.isEmpty())
        return false;
    const uint64_t rowBytes = uint64_t(size.width()) * 4;
    const int64_t stride = data.chunk->stride;
    if (stride < int64_t(rowBytes) || stride % 4 != 0)
        return false;
    const uint64_t offset = data.chunk->offset % data.maxsize;
    const uint64_t bytes = uint64_t(size.height() - 1) * stride + rowBytes;
    return bytes <= data.chunk->size && offset + bytes <= data.maxsize;
}
