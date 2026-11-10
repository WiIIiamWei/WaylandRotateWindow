#include "view.hpp"
#include <QtTest>
#include <QApplication>
#include <QSurfaceFormat>

class TestView : public View {
public:
    using View::View;
    QImage render()
    {
        makeCurrent();
        paintGL();
        const auto result = grabFramebuffer();
        doneCurrent();
        return result;
    }
};

class RenderTest : public QObject {
    Q_OBJECT
private slots:
    void pixels()
    {
        Capture capture;
        TestView view(&capture);
        QSignalSpy errors(&view, &View::failed);
        view.resize(160, 160);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTRY_VERIFY(view.isReady());
        QImage image(80, 80, QImage::Format_RGBA8888);
        const QColor colors[] = {Qt::red, Qt::green, Qt::blue, Qt::yellow};
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 80; ++x)
                image.setPixelColor(x, y, colors[(y >= 40) * 2 + (x >= 40)]);
        spa_chunk chunk{0, uint32_t(image.sizeInBytes()), int32_t(image.bytesPerLine()), 0};
        spa_data data{};
        data.type = SPA_DATA_MemPtr;
        data.data = image.bits();
        data.maxsize = image.sizeInBytes();
        data.chunk = &chunk;
        spa_buffer buffer{};
        buffer.n_datas = 1;
        buffer.datas = &data;
        pw_buffer pwBuffer{};
        pwBuffer.buffer = &buffer;
        spa_video_info_raw format{};
        format.format = SPA_VIDEO_FORMAT_RGBA;
        format.size = {80, 80};
        view.present({&pwBuffer, format, QRect(0, 0, 80, 80)});
        const int expected[4][4][4] = {
            {{0,1,2,3}, {1,0,3,2}, {2,3,0,1}, {3,2,1,0}},
            {{2,0,3,1}, {0,2,1,3}, {3,1,2,0}, {1,3,0,2}},
            {{3,2,1,0}, {2,3,0,1}, {1,0,3,2}, {0,1,2,3}},
            {{1,3,0,2}, {3,1,2,0}, {0,2,1,3}, {2,0,3,1}}
        };
        for (int rotation = 0; rotation < 4; ++rotation) {
            for (int flips = 0; flips < 4; ++flips) {
                view.setTransform(rotation * 90, flips & 1, flips & 2);
                const QImage rendered = view.render();
                QVERIFY(!rendered.isNull());
                for (int i = 0; i < 4; ++i) {
                    const int x = rendered.width() * (i % 2 ? 3 : 1) / 4;
                    const int y = rendered.height() * (i / 2 ? 3 : 1) / 4;
                    QCOMPARE(rendered.pixelColor(x, y), colors[expected[rotation][flips][i]]);
                }
            }
        }
        // Crop and a resized, padded BGRA frame exercise upload and aspect handling.
        QImage resized(120, 80, QImage::Format_ARGB32);
        resized.fill(Qt::magenta);
        data.data = resized.bits();
        data.maxsize = resized.sizeInBytes();
        chunk.size = resized.sizeInBytes();
        chunk.stride = resized.bytesPerLine();
        format.format = SPA_VIDEO_FORMAT_BGRA;
        format.size = {100, 80};
        view.setTransform(90, false, false);
        view.present({&pwBuffer, format, QRect(10, 10, 60, 40)});
        auto rendered = view.render();
        QCOMPARE(rendered.pixelColor(rendered.width() / 2, rendered.height() / 2), QColor(Qt::magenta));
        QVERIFY(rendered.pixelColor(0, 0).red() < 10);
        view.clear();
        rendered = view.render();
        QVERIFY(rendered.pixelColor(rendered.width() / 2, rendered.height() / 2).red() < 10);
        QCOMPARE(errors.count(), 0);
        QSignalSpy fallback(&view, &View::dmaImportFailed);
        data.type = SPA_DATA_DmaBuf;
        data.fd = -1;
        format.flags = SPA_VIDEO_FLAG_MODIFIER;
        view.present({&pwBuffer, format, QRect(0, 0, 100, 80)});
        view.render();
        QCOMPARE(fallback.count(), 1);
    }
};

int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    RenderTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_render.moc"
