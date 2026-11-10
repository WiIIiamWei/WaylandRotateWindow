#pragma once

#include "capture.hpp"
#include <QOpenGLWindow>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <memory>

class View : public QOpenGLWindow, protected QOpenGLExtraFunctions {
    Q_OBJECT
public:
    explicit View(Capture *capture);
    ~View() override;
    void present(Frame frame);
    void setTransform(int degrees, bool x, bool y);
    void clear();
    const std::vector<DmaFormat> &dmaFormats() const { return supportedDma; }
    bool isReady() const { return initialized; }

signals:
    void rendererReady();
    void dmaImportFailed();
    void failed(QString message);

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    bool upload(Frame frame);
    bool importDma(Frame frame);
    void retireCurrent(bool returnBuffer = true);
    void removed(pw_buffer *buffer);
    void findDmaFormats();
    Capture *capture;
    Frame pending;
    Frame current;
    int rotation = 0;
    bool flipX = false, flipY = false;
    bool initialized = false;
    bool dmaRetryPending = false;
    std::unique_ptr<QOpenGLShaderProgram> shader;
    GLuint vao = 0, vbo = 0, texture = 0;
    QSize textureSize;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLsync fence = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    using BindImage = void (*)(GLenum, void *);
    BindImage bindImage = nullptr;
    std::vector<DmaFormat> supportedDma;
};
