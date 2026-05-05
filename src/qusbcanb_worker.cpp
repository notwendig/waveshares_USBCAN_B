#include "qusbcanb_worker.h"

namespace QUsbCanB {

Worker::Worker(QObject* parent)
    : QObject(parent)
{
    m_rxTimer = new QTimer(this);
    connect(m_rxTimer, &QTimer::timeout, this, &Worker::pollRx);

    m_txTimer = new QTimer(this);
    m_txTimer->setInterval(0);
    m_txTimer->setSingleShot(false);
    connect(m_txTimer, &QTimer::timeout, this, &Worker::flushTx);
}

Worker::~Worker()
{
    stop();
}

void Worker::start(int channel, quint32 bitrate, quint8 mode, int pollIntervalMs)
{
    if (m_running)
        return;

    m_channel = channel;
    QString err;
    if (!m_device.open(&err)) {
        emit errorText(err);
        return;
    }
    const bool ok = (m_channel == 2)
        ? m_device.configureBothAndStart(bitrate, mode, &err)
        : m_device.configureAndStart(m_channel, bitrate, mode, &err);
    if (!ok) {
        emit errorText(err);
        m_device.close();
        return;
    }

    m_running = true;
    m_rxTimer->start(qMax(1, pollIntervalMs));
    if (!m_txQueue.isEmpty())
        m_txTimer->start();
    emit opened();
}

void Worker::stop()
{
    if (m_rxTimer)
        m_rxTimer->stop();
    if (m_txTimer)
        m_txTimer->stop();
    m_txQueue.clear();
    if (m_device.isOpen())
        m_device.close();
    if (m_running)
        emit closed();
    m_running = false;
}

void Worker::sendFrame(const QCanBusFrame& frame)
{
    if (!m_running)
        return;

    if (m_txQueue.size() >= m_txQueueLimit) {
        emit framesDropped(1);
        emit errorText(QStringLiteral("TX queue full; dropping frame"));
        return;
    }

    m_txQueue.enqueue(frame);
    if (!m_txTimer->isActive())
        m_txTimer->start();
}

QList<QCanBusFrame> Worker::takeTxBatch()
{
    QList<QCanBusFrame> out;
    const int n = qMin(m_txBatchSize, m_txQueue.size());
    out.reserve(n);
    for (int i = 0; i < n; ++i)
        out.append(m_txQueue.dequeue());
    return out;
}

void Worker::requeueFront(const QList<QCanBusFrame>& frames, int first)
{
    for (int i = frames.size() - 1; i >= first; --i)
        m_txQueue.prepend(frames.at(i));
}

void Worker::flushTx()
{
    if (!m_running)
        return;

    if (m_txQueue.isEmpty()) {
        m_txTimer->stop();
        return;
    }

    const QList<QCanBusFrame> batch = takeTxBatch();
    if (batch.isEmpty()) {
        m_txTimer->stop();
        return;
    }

    QString err;
    const int sent = m_device.writeFrames(m_channel, batch, &err);

    if (sent > 0)
        emit framesWritten(sent);

    if (sent == batch.size()) {
        if (m_txQueue.isEmpty())
            m_txTimer->stop();
        return;
    }

    if (sent > 0 && sent < batch.size()) {
        // Short accept: keep remaining frames, repack on next wake-up.
        requeueFront(batch, sent);
        if (!err.isEmpty())
            emit errorText(QStringLiteral("Partial TX accept: %1/%2; repacking remaining frames. Last error: %3")
                               .arg(sent).arg(batch.size()).arg(err));
        return;
    }

    // USB-level hard failure before the adapter accepted any frame: log & forget this batch.
    emit framesDropped(batch.size());
    if (!err.isEmpty())
        emit errorText(QStringLiteral("TX batch dropped after USB error: %1").arg(err));

    if (m_txQueue.isEmpty())
        m_txTimer->stop();
}

void Worker::pollRx()
{
    if (!m_running)
        return;
    QString err;
    const auto frames = m_device.readFrames(m_channel, 1, &err);
    if (!frames.isEmpty())
        emit framesReceived(frames);
    if (!err.isEmpty())
        emit errorText(err);
}

} // namespace QUsbCanB
