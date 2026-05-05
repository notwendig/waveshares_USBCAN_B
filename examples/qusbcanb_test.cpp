#include "qusbcanbusdevice.h"
#include "qusbcanb_lowlevel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCommandLineParser>
#include <QtCore/QTimer>
#include <QtCore/QDebug>
#include <QtCore/QVariant>
#include <iostream>

using namespace QUsbCanB;

static QByteArray makePayload(int seq)
{
    QByteArray p;
    p.append(char(0x11));
    p.append(char(0x22));
    p.append(char(0x33));
    p.append(char(0x44));
    p.append(char(seq & 0xff));
    p.append(char(0x66));
    p.append(char(0x77));
    p.append(char(0x88));
    return p;
}

static QString dataHex(const QByteArray& data)
{
    QString out;
    for (unsigned char c : data)
        out += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'));
    return out;
}

static int lowLevelInitOnly(int channel, bool selfTest)
{
    LowLevelDevice dev;
    QString err;
    if (!dev.open(&err)) {
        std::cerr << err.toStdString() << "\n";
        return 1;
    }
    const bool ok = dev.configureAndStart(channel, 125000, selfTest ? 2 : 0, &err);
    if (!ok) {
        std::cerr << "CAN" << channel << " init failed: " << err.toStdString() << "\n";
        return 2;
    }
    std::cout << "CAN" << channel << " init/start OK\n";
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<QCanBusFrame>("QCanBusFrame");
    qRegisterMetaType<QList<QCanBusFrame>>("QList<QCanBusFrame>");

    QCommandLineParser parser;
    parser.setApplicationDescription("QUsbCanB v19 thread-safe Qt/QCanBusDevice test");
    parser.addHelpOption();
    QCommandLineOption initOnly(QStringLiteral("init-only"), QStringLiteral("Only initialize/start the selected channel."));
    QCommandLineOption selfTest(QStringLiteral("self-test"), QStringLiteral("Use ControlCAN self-test mode 2."));
    QCommandLineOption countOpt(QStringLiteral("count"), QStringLiteral("Frames to send."), QStringLiteral("n"), QStringLiteral("10"));
    QCommandLineOption channelOpt(QStringLiteral("channel"), QStringLiteral("CAN channel 1 or 2."), QStringLiteral("ch"), QStringLiteral("1"));
    parser.addOption(initOnly);
    parser.addOption(selfTest);
    parser.addOption(countOpt);
    parser.addOption(channelOpt);
    parser.process(app);

    const int channel = parser.value(channelOpt).toInt() == 2 ? 2 : 1;
    const int count = qMax(0, parser.value(countOpt).toInt());
    const bool modeSelfTest = parser.isSet(selfTest);

    const auto ep = LowLevelDevice::endpointsForChannel(channel);
    std::cout << "QUsbCanB v19: channel=" << channel
              << ", bitrate=125000, mode=" << (modeSelfTest ? "self-test" : "normal") << "\n";
    std::cout << "CAN" << channel << " endpoints: CMD OUT 0x" << std::hex << int(ep.commandOut)
              << ", TX OUT 0x" << int(ep.txOut)
              << ", RX IN 0x" << int(ep.rxIn) << std::dec << "\n";

    if (parser.isSet(initOnly))
        return lowLevelInitOnly(channel, modeSelfTest);

    QUsbCanBusDevice dev(channel);
    dev.setConfigurationParameter(QCanBusDevice::BitRateKey, QVariant::fromValue(125000));
    dev.setConfigurationParameter(QCanBusDevice::LoopbackKey, QVariant::fromValue(modeSelfTest));
    dev.setPollInterval(2);

    int rx = 0;
    int written = 0;
    QObject::connect(&dev, &QCanBusDevice::errorOccurred, [&dev](QCanBusDevice::CanBusError e) {
        if (e != QCanBusDevice::NoError)
            std::cerr << "QCanBus error: " << dev.errorString().toStdString() << "\n";
    });
    QObject::connect(&dev, &QCanBusDevice::framesWritten, [&](qint64 n) {
        written += int(n);
        std::cout << "framesWritten=" << n << " total=" << written << "\n";
    });
    QObject::connect(&dev, &QCanBusDevice::framesReceived, [&]() {
        while (dev.framesAvailable() > 0) {
            const QCanBusFrame f = dev.readFrame();
            ++rx;
            std::cout << "RX CAN" << channel
                      << " id=0x" << std::hex << f.frameId() << std::dec
                      << " dlc=" << f.payload().size()
                      << " data=" << dataHex(f.payload()).toStdString() << "\n";
        }
    });
    QObject::connect(&dev, &QCanBusDevice::stateChanged, [&](QCanBusDevice::CanBusDeviceState state) {
        if (state == QCanBusDevice::ConnectedState) {
            std::cout << "connected\n";
            for (int i = 0; i < count; ++i) {
                QCanBusFrame f(channel == 1 ? 0x121 : 0x122, makePayload(i));
                dev.writeFrame(f);
                std::cout << "TX queued seq=" << i << "\n";
            }
            QTimer::singleShot(1500, &app, [&]() {
                std::cout << "summary: queued=" << count << ", written=" << written << ", rx=" << rx << "\n";
                app.quit();
            });
        }
    });

    if (!dev.connectDevice()) {
        std::cerr << "connectDevice() failed: " << dev.errorString().toStdString() << "\n";
        return 1;
    }

    return app.exec();
}
