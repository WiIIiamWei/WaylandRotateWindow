#include "frame.hpp"
#include <QtTest>

class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void transforms()
    {
        const QPointF corners[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        const int expected[4][4][4] = {
            {{0,1,2,3}, {1,0,3,2}, {2,3,0,1}, {3,2,1,0}},
            {{2,0,3,1}, {0,2,1,3}, {3,1,2,0}, {1,3,0,2}},
            {{3,2,1,0}, {2,3,0,1}, {1,0,3,2}, {0,1,2,3}},
            {{1,3,0,2}, {3,1,2,0}, {0,2,1,3}, {2,0,3,1}}
        };
        for (int r = 0; r < 4; ++r)
            for (int flips = 0; flips < 4; ++flips)
                for (int i = 0; i < 4; ++i)
                    QCOMPARE(sourceUv(corners[i], r * 90, flips & 1, flips & 2),
                             corners[expected[r][flips][i]]);
        QCOMPARE(rotatedSize({640, 480}, 90), QSize(480, 640));
        QCOMPARE(rotatedSize({640, 480}, 180), QSize(640, 480));
    }

    void shmBounds()
    {
        uint8_t pixels[128]{};
        spa_chunk chunk{8, 40, 24, 0};
        spa_data data{};
        data.data = pixels;
        data.chunk = &chunk;
        data.maxsize = sizeof(pixels);
        QVERIFY(validShm(data, {4, 2}));
        chunk.size = 39;
        QVERIFY(!validShm(data, {4, 2}));
        chunk.size = 40;
        chunk.offset = 100;
        QVERIFY(!validShm(data, {4, 2}));
        chunk.offset = 136;
        QVERIFY(validShm(data, {4, 2}));
        chunk.stride = -24;
        QVERIFY(!validShm(data, {4, 2}));
        chunk.stride = 15;
        QVERIFY(!validShm(data, {4, 2}));
        chunk.stride = 17;
        QVERIFY(!validShm(data, {4, 2}));
        chunk.stride = 24;
        data.maxsize = 0;
        QVERIFY(!validShm(data, {4, 2}));
        data.maxsize = sizeof(pixels);
        data.data = nullptr;
        QVERIFY(!validShm(data, {4, 2}));
        QVERIFY(!validShm(data, {0, 2}));
    }

    void cropBounds()
    {
        spa_buffer buffer{};
        QCOMPARE(frameCrop(&buffer, {640, 480}).value(), QRect(0, 0, 640, 480));
        spa_meta_region crop{{{10, 20}, {100, 200}}};
        spa_meta meta{SPA_META_VideoCrop, sizeof(crop), &crop};
        buffer.n_metas = 1;
        buffer.metas = &meta;
        QCOMPARE(frameCrop(&buffer, {640, 480}).value(), QRect(10, 20, 100, 200));
        crop.region.position.x = -1;
        QVERIFY(!frameCrop(&buffer, {640, 480}));
        crop.region.position.x = 600;
        QVERIFY(!frameCrop(&buffer, {640, 480}));
        crop.region.size = {0, 0};
        QCOMPARE(frameCrop(&buffer, {640, 480}).value(), QRect(0, 0, 640, 480));
    }
};

QTEST_APPLESS_MAIN(CoreTest)
#include "test_core.moc"
