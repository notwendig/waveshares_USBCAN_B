#include "qusbcanbusdevice.h"
#include <QVariant>
#include <QtCore/QMetaObject>
#include <QtCore/QMetaType>
#include <QtCore/QMutexLocker>

namespace QUsbCanB {

QUsbCanBusDevice::QUsbCanBusDevice(int channel, QObject* parent)
    : QCanBusDevice(parent), m_channel(channel == 2 ? 2 : 1)
{
    qRegisterMetaType<QCanBusFrame>("QCanBusFrame");
    qRegisterMetaType<QList<QCanBusFrame>>("QList<QCanBusFrame>");
}

QUsbCanBusDevice::~QUsbCanBusDevice()
{
    if (state() != QCanBusDevice::UnconnectedState)
        close();
}

void QUsbCanBusDevice::setChannel(int channel)
{
    QMutexLocker locker(&m_apiMutex);
    if (state() == QCanBusDevice::UnconnectedState)
        m_channel = (channel == 2 ? 2 : 1);
}

quint32 QUsbCanBusDevice::bitrateFromConfiguration() const
{
    const QVariant v = configurationParameter(QCanBusDevice::BitRateKey);
    if (v.isValid() && v.toUInt() > 0)
        return v.toUInt();
    return 125000;
}

quint8 QUsbCanBusDevice::modeFromConfiguration() const
{
    if (configurationParameter(QCanBusDevice::LoopbackKey).toBool())
        return 2; // ControlCAN self-test mode
    if (configurationParameter(QCanBusDevice::ReceiveOwnKey).toBool())
        return 2;
    return 0;
}

bool QUsbCanBusDevice::open()
{
    if (m_worker)
        return true;

    setState(QCanBusDevice::ConnectingState);

    m_worker = new Worker();
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &Worker::opened, this, &QUsbCanBusDevice::onOpened, Qt::QueuedConnection);
    connect(m_worker, &Worker::closed, this, &QUsbCanBusDevice::onClosed, Qt::QueuedConnection);
    connect(m_worker, &Worker::errorText, this, &QUsbCanBusDevice::onError, Qt::QueuedConnection);
    connect(m_worker, &Worker::framesReceived, this, &QUsbCanBusDevice::onFramesReceived, Qt::QueuedConnection);
    connect(m_worker, &Worker::framesWritten, this, &QUsbCanBusDevice::onFramesWritten, Qt::QueuedConnection);

    m_thread.start();
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection,
                              Q_ARG(int, m_channel),
                              Q_ARG(quint32, bitrateFromConfiguration()),
                              Q_ARG(quint8, modeFromConfiguration()),
                              Q_ARG(int, m_pollIntervalMs));
    return true;
}

void QUsbCanBusDevice::close()
{
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    m_worker = nullptr;
    setState(QCanBusDevice::UnconnectedState);
}

bool QUsbCanBusDevice::writeFrame(const QCanBusFrame& frame)
{
    QMutexLocker locker(&m_apiMutex);
    if (!m_worker || state() != QCanBusDevice::ConnectedState) {
        setError(QStringLiteral("QUsbCanB device is not connected"), QCanBusDevice::ConnectionError);
        return false;
    }
    QMetaObject::invokeMethod(m_worker, "sendFrame", Qt::QueuedConnection, Q_ARG(QCanBusFrame, frame));
    return true;
}

QString QUsbCanBusDevice::interpretErrorFrame(const QCanBusFrame& errorFrame)
{
    Q_UNUSED(errorFrame)
    return QStringLiteral("QUsbCanB does not decode CAN error frames yet");
}

void QUsbCanBusDevice::onOpened()
{
    setState(QCanBusDevice::ConnectedState);
}

void QUsbCanBusDevice::onClosed()
{
    setState(QCanBusDevice::UnconnectedState);
}

void QUsbCanBusDevice::onError(const QString& text)
{
    setError(text, QCanBusDevice::ReadError);
}

void QUsbCanBusDevice::onFramesReceived(const QList<QCanBusFrame>& frames)
{
    enqueueReceivedFrames(frames);
}

void QUsbCanBusDevice::onFramesWritten(qint64 count)
{
    emit framesWritten(count);
}

} // namespace QUsbCanB
