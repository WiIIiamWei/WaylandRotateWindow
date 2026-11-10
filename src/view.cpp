#include "view.hpp"

#include <QOpenGLContext>
#include <drm_fourcc.h>
#include <algorithm>
#include <climits>

View::View(Capture *source) : QOpenGLWindow(NoPartialUpdate), capture(source)
{
    connect(capture, &Capture::aboutToStop, this, &View::clear);
    connect(capture, &Capture::bufferRemoved, this, &View::removed);
    connect(capture, &Capture::frameReady, this, [this] { present(capture->takeFrame()); });
}

View::~View()
{
    clear();
    if (initialized) {
        makeCurrent();
        glDeleteTextures(1, &texture);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        shader.reset();
        doneCurrent();
    }
}

void View::initializeGL()
{
    initializeOpenGLFunctions();
    if (context()->isOpenGLES() || context()->format().majorVersion() < 3 ||
        (context()->format().majorVersion() == 3 && context()->format().minorVersion() < 3)) {
        emit failed(tr("需要支持 OpenGL 3.3 的显卡驱动。"));
        return;
    }
    shader = std::make_unique<QOpenGLShaderProgram>();
    if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex,
        "#version 330 core\n"
        "layout(location=0) in vec2 position;\n"
        "layout(location=1) in vec2 uv;\n"
        "out vec2 texCoord;\n"
        "void main() { gl_Position=vec4(position,0,1); texCoord=uv; }\n") ||
        !shader->addShaderFromSourceCode(QOpenGLShader::Fragment,
        "#version 330 core\n"
        "in vec2 texCoord; uniform sampler2D frame; out vec4 color;\n"
        "void main() { color=vec4(texture(frame,texCoord).rgb,1); }\n") || !shader->link()) {
        emit failed(tr("GPU 着色器初始化失败：%1").arg(shader->log()));
        shader.reset();
        return;
    }
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void *>(2 * sizeof(float)));
    findDmaFormats();
    initialized = true;
    emit rendererReady();
}

void View::findDmaFormats()
{
    display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || !context()->hasExtension("GL_OES_EGL_image"))
        return;
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions || !QByteArray(extensions).split(' ').contains("EGL_EXT_image_dma_buf_import"))
        return;
    createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    bindImage = reinterpret_cast<BindImage>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!createImage || !destroyImage || !bindImage)
        return;
    auto queryModifiers = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    const bool hasModifiers = QByteArray(extensions).split(' ').contains("EGL_EXT_image_dma_buf_import_modifiers");
    const DmaFormat formats[] = {
        {SPA_VIDEO_FORMAT_BGRA, DRM_FORMAT_ARGB8888, {}},
        {SPA_VIDEO_FORMAT_BGRx, DRM_FORMAT_XRGB8888, {}},
        {SPA_VIDEO_FORMAT_RGBA, DRM_FORMAT_ABGR8888, {}},
        {SPA_VIDEO_FORMAT_RGBx, DRM_FORMAT_XBGR8888, {}}
    };
    for (auto format : formats) {
        if (hasModifiers && queryModifiers) {
            EGLint count = 0;
            if (queryModifiers(display, format.fourcc, 0, nullptr, nullptr, &count) && count > 0 && count <= 1024) {
                std::vector<EGLuint64KHR> modifiers(count);
                std::vector<EGLBoolean> externalOnly(count);
                if (queryModifiers(display, format.fourcc, count, modifiers.data(), externalOnly.data(), &count))
                    for (int i = 0; i < count; ++i)
                        if (!externalOnly[i])
                            format.modifiers.push_back(modifiers[i]);
            }
        }
        // Implicit modifiers also work on drivers without the modifiers extension.
        format.modifiers.push_back(DRM_FORMAT_MOD_INVALID);
        supportedDma.push_back(std::move(format));
    }
}

void View::present(Frame frame)
{
    capture->release(pending.buffer);
    pending = frame;
    requestUpdate();
}

void View::setTransform(int degrees, bool x, bool y)
{
    rotation = degrees;
    flipX = x;
    flipY = y;
    requestUpdate();
}

void View::retireCurrent(bool returnBuffer)
{
    if (fence) {
        const GLenum result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
        if (result == GL_TIMEOUT_EXPIRED || result == GL_WAIT_FAILED)
            glFinish();
        glDeleteSync(fence);
        fence = nullptr;
    } else if (image != EGL_NO_IMAGE_KHR) {
        glFinish();
    }
    if (image != EGL_NO_IMAGE_KHR) {
        destroyImage(display, image);
        image = EGL_NO_IMAGE_KHR;
        glDeleteTextures(1, &texture);
        texture = 0;
        textureSize = {};
    }
    if (returnBuffer)
        capture->release(current.buffer);
    current = {};
}

bool View::importDma(Frame frame)
{
    const auto found = std::find_if(supportedDma.begin(), supportedDma.end(), [&](const auto &item) {
        return item.format == frame.format.format;
    });
    auto *buffer = frame.buffer->buffer;
    if (found == supportedDma.end() || !buffer->n_datas || buffer->n_datas > 4)
        return false;
    std::vector<EGLint> attributes{EGL_WIDTH, EGLint(frame.format.size.width),
        EGL_HEIGHT, EGLint(frame.format.size.height), EGL_LINUX_DRM_FOURCC_EXT, EGLint(found->fourcc)};
    const EGLint fdKeys[] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                            EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    const EGLint offsetKeys[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    const EGLint pitchKeys[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                               EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    const EGLint loKeys[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    const EGLint hiKeys[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    for (uint32_t i = 0; i < buffer->n_datas; ++i) {
        const auto &data = buffer->datas[i];
        if (data.type != SPA_DATA_DmaBuf || data.fd < 0 || data.fd > INT_MAX || !data.chunk ||
            data.chunk->stride <= 0 || uint64_t(data.mapoffset) + data.chunk->offset > INT_MAX)
            return false;
        attributes.insert(attributes.end(), {fdKeys[i], EGLint(data.fd), offsetKeys[i],
            EGLint(data.mapoffset + data.chunk->offset), pitchKeys[i], data.chunk->stride});
        if (frame.format.modifier != DRM_FORMAT_MOD_INVALID)
            attributes.insert(attributes.end(), {loKeys[i], EGLint(frame.format.modifier & 0xffffffff),
                                                hiKeys[i], EGLint(frame.format.modifier >> 32)});
    }
    attributes.push_back(EGL_NONE);
    image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
    if (image == EGL_NO_IMAGE_KHR)
        return false;
    if (texture)
        glDeleteTextures(1, &texture);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetError();
    bindImage(GL_TEXTURE_2D, image);
    return glGetError() == GL_NO_ERROR;
}

bool View::upload(Frame frame)
{
    auto *buffer = frame.buffer->buffer;
    auto &data = buffer->datas[0];
    current = frame;
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (frame.format.size.width > uint32_t(maxSize) || frame.format.size.height > uint32_t(maxSize)) {
        retireCurrent();
        emit failed(tr("目标窗口的尺寸超过 GPU 纹理上限。"));
        return false;
    }
    if (data.type == SPA_DATA_DmaBuf) {
        if (!importDma(frame)) {
            retireCurrent();
            if (!dmaRetryPending) {
                dmaRetryPending = true;
                emit dmaImportFailed();
            }
            return false;
        }
    } else {
        const QSize size(frame.format.size.width, frame.format.size.height);
        if ((data.type != SPA_DATA_MemFd && data.type != SPA_DATA_MemPtr) || !validShm(data, size)) {
            retireCurrent();
            emit failed(tr("视频帧的共享内存布局无效。"));
            return false;
        }
        if (!texture)
            glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, data.chunk->stride / 4);
        const bool bgr = frame.format.format == SPA_VIDEO_FORMAT_BGRA || frame.format.format == SPA_VIDEO_FORMAT_BGRx;
        const auto *pixels = static_cast<const uint8_t *>(data.data) + data.chunk->offset % data.maxsize;
        if (textureSize != size) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0,
                         bgr ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            textureSize = size;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.width(), size.height(),
                            bgr ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (glGetError() != GL_NO_ERROR) {
            retireCurrent();
            textureSize = {};
            emit failed(tr("GPU 视频纹理上传失败，请检查驱动和可用显存。"));
            return false;
        }
        // Client-memory uploads finish reading the pointer before returning.
        capture->release(current.buffer);
        current.buffer = nullptr;
        dmaRetryPending = false;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void View::paintGL()
{
    glClearColor(0.025f, 0.025f, 0.025f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!initialized || width() <= 0 || height() <= 0)
        return;
    if (pending.buffer) {
        retireCurrent();
        const Frame frame = pending;
        pending = {};
        if (!upload(frame))
            return;
    }
    if (current.crop.isEmpty() || !texture)
        return;
    const QSize content = rotatedSize(current.crop.size(), rotation);
    const double scale = std::min(double(width()) / content.width(), double(height()) / content.height());
    const float sx = content.width() * scale / width(), sy = content.height() * scale / height();
    const QPointF corners[] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    float vertices[16];
    for (int i = 0; i < 4; ++i) {
        const auto uv = sourceUv(corners[i], rotation, flipX, flipY);
        vertices[i * 4] = corners[i].x() ? sx : -sx;
        vertices[i * 4 + 1] = corners[i].y() ? -sy : sy;
        vertices[i * 4 + 2] = (current.crop.x() + uv.x() * current.crop.width()) / current.format.size.width;
        vertices[i * 4 + 3] = (current.crop.y() + uv.y() * current.crop.height()) / current.format.size.height;
    }
    shader->bind();
    shader->setUniformValue("frame", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    shader->release();
    if (image != EGL_NO_IMAGE_KHR) {
        if (fence)
            glDeleteSync(fence);
        fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
    }
}

void View::clear()
{
    capture->release(pending.buffer);
    pending = {};
    if (initialized) {
        makeCurrent();
        retireCurrent();
        doneCurrent();
    }
    dmaRetryPending = false;
    requestUpdate();
}

void View::removed(pw_buffer *buffer)
{
    if (pending.buffer == buffer)
        pending = {};
    if (current.buffer == buffer && initialized) {
        makeCurrent();
        // The producer is removing this buffer; it must not be queued again.
        retireCurrent(false);
        doneCurrent();
        requestUpdate();
    }
}
