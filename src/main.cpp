#include "portal.hpp"
#include "capture.hpp"
#include "view.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QVBoxLayout>

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "wayland");
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    app.setApplicationName("Wayland Rotate Window");
    app.setApplicationVersion("1.0.0");
    app.setDesktopFileName("io.github.WiIIiamWei.WaylandRotateWindow");
    pw_init(nullptr, nullptr);

    Capture capture;
    Portal portal;
    QWidget window;
    window.setWindowTitle(QObject::tr("Wayland 窗口旋转"));
    window.resize(960, 640);
    auto *view = new View(&capture);
    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 8);
    auto *container = QWidget::createWindowContainer(view);
    container->setMinimumSize(320, 200);
    layout->addWidget(container, 1);
    auto *controls = new QHBoxLayout;
    controls->setContentsMargins(12, 4, 12, 0);
    auto *select = new QPushButton(QObject::tr("选择窗口"));
    select->setEnabled(false);
    auto *stop = new QPushButton(QObject::tr("停止"));
    stop->setEnabled(false);
    auto *rotation = new QComboBox;
    rotation->addItems({"0°", "90°", "180°", "270°"});
    rotation->setAccessibleName(QObject::tr("顺时针旋转角度"));
    auto *flipX = new QCheckBox(QObject::tr("翻转 X（左右）"));
    auto *flipY = new QCheckBox(QObject::tr("翻转 Y（上下）"));
    controls->addWidget(select);
    controls->addWidget(stop);
    controls->addSpacing(12);
    controls->addWidget(new QLabel(QObject::tr("旋转：")));
    controls->addWidget(rotation);
    controls->addWidget(flipX);
    controls->addWidget(flipY);
    controls->addStretch();
    layout->addLayout(controls);
    auto *status = new QLabel(QObject::tr("选择一个窗口以实时旋转或翻转画面。"));
    status->setContentsMargins(12, 0, 12, 0);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);

    auto transform = [=] { view->setTransform(rotation->currentIndex() * 90, flipX->isChecked(), flipY->isChecked()); };
    QObject::connect(rotation, &QComboBox::currentIndexChanged, &window, transform);
    QObject::connect(flipX, &QCheckBox::toggled, &window, transform);
    QObject::connect(flipY, &QCheckBox::toggled, &window, transform);
    QObject::connect(view, &View::rendererReady, select, [select] { select->setEnabled(true); });
    QObject::connect(select, &QPushButton::clicked, &portal, &Portal::start);
    QObject::connect(&portal, &Portal::selecting, &window, [&] {
        select->setEnabled(false);
        stop->setEnabled(true);
    });
    QObject::connect(stop, &QPushButton::clicked, &portal, [&] {
        portal.stop();
        status->setText(QObject::tr("已停止采集。"));
    });
    QObject::connect(&portal, &Portal::ready, &capture, [&](QDBusUnixFileDescriptor fd, uint32_t node, const QString &serial) {
        capture.start(fd, node, serial, view->dmaFormats());
        select->setEnabled(true);
    });
    QObject::connect(&portal, &Portal::ended, &capture, [&] {
        capture.stop();
        select->setEnabled(view->isReady());
        stop->setEnabled(false);
    });
    QObject::connect(&portal, &Portal::status, status, &QLabel::setText);
    QObject::connect(&capture, &Capture::status, status, &QLabel::setText);
    QObject::connect(view, &View::dmaImportFailed, &capture, &Capture::retryShm, Qt::QueuedConnection);
    auto failure = [&](const QString &message) { portal.stop(); status->setText(message); };
    QObject::connect(&capture, &Capture::failed, &portal, failure, Qt::QueuedConnection);
    QObject::connect(view, &View::failed, &portal, failure, Qt::QueuedConnection);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &portal, [&] { portal.stop(); capture.stop(); });
    window.show();
    const int result = app.exec();
    portal.stop();
    capture.stop();
    pw_deinit();
    return result;
}
