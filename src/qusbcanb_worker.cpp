#include "qusbcanb_worker.h"

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <vector>

namespace QUsbCanB {

Worker::Worker(QObject *parent)
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

    m_channel = channel == 2 ? 2 : 1;

    if (!m_device.open()) {
        emit errorText(QString::fromStdString(m_device.lastError()));
        return;
    }

    if (!m_device.configure(channelFromInt(m_channel), bitrate, modeFromValue(mode))) {
        emit errorText(QString::fromStdString(m_device.lastError()));
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

    if (m_device.isOpen()) {
        (void)m_device.stop(channelFromInt(m_channel));
        m_device.close();
    }

    if (m_running)
        emit closed();
    m_running = false;
}

void Worker::sendFrame(const QCanBusFrame &frame)
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

void Worker::requeueFront(const QList<QCanBusFrame> &frames, int first)
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

    std::vector<qusbcanb::CanFrame> frames;
    frames.reserve(static_cast<std::size_t>(batch.size()));
    for (const QCanBusFrame &frame : batch)
        frames.push_back(toLowLevelFrame(frame));

    if (m_device.send(channelFromInt(m_channel), frames)) {
        emit framesWritten(batch.size());
        if (m_txQueue.isEmpty())
            m_txTimer->stop();
        return;
    }

    emit framesDropped(batch.size());
    emit errorText(QStringLiteral("TX batch dropped after USB error: %1")
                   .arg(QString::fromStdString(m_device.lastError())));
    if (m_txQueue.isEmpty())
        m_txTimer->stop();
}

void Worker::pollRx()
{
    if (!m_running)
        return;

    std::vector<qusbcanb::CanFrame> lowLevelFrames;
    if (!m_device.receive(channelFromInt(m_channel), lowLevelFrames, 256, std::chrono::milliseconds{1})) {
        emit errorText(QString::fromStdString(m_device.lastError()));
        return;
    }

    if (lowLevelFrames.empty())
        return;

    QList<QCanBusFrame> qtFrames;
    qtFrames.reserve(static_cast<int>(lowLevelFrames.size()));
    for (const qusbcanb::CanFrame &frame : lowLevelFrames)
        qtFrames.append(toQtFrame(frame));

    emit framesReceived(qtFrames);
}

qusbcanb::Channel Worker::channelFromInt(int channel) noexcept
{
    return channel == 2 ? qusbcanb::Channel::Can2 : qusbcanb::Channel::Can1;
}

qusbcanb::CanMode Worker::modeFromValue(quint8 mode) noexcept
{
    if (mode == 2)
        return qusbcanb::CanMode::SelfTest;
    if (mode == 1)
        return qusbcanb::CanMode::ListenOnly;
    return qusbcanb::CanMode::Normal;
}

qusbcanb::CanFrame Worker::toLowLevelFrame(const QCanBusFrame &frame)
{
    qusbcanb::CanFrame result;
    result.id = static_cast<std::uint32_t>(frame.frameId());
    result.extended = frame.hasExtendedFrameFormat();
    result.remote = frame.frameType() == QCanBusFrame::RemoteRequestFrame;

    const QByteArray payload = frame.payload();
    const int len = std::min<int>(static_cast<int>(payload.size()), 8);
    result.dlc = static_cast<std::uint8_t>(len);
    for (int i = 0; i < len; ++i)
        result.data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(payload.at(i));
    return result;
}

QCanBusFrame Worker::toQtFrame(const qusbcanb::CanFrame &frame)
{
    QCanBusFrame result;
    result.setFrameId(frame.id);
    result.setExtendedFrameFormat(frame.extended);
    result.setFrameType(frame.remote ? QCanBusFrame::RemoteRequestFrame : QCanBusFrame::DataFrame);

    QByteArray payload;
    payload.resize(static_cast<int>(std::min<std::uint8_t>(frame.dlc, 8)));
    for (int i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>(frame.data[static_cast<std::size_t>(i)]);
    result.setPayload(payload);
    return result;
}

} // namespace QUsbCanB
