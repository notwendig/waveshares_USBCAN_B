#pragma once

#include "qusbcanb_lowlevel.h"

#include <QCanBusFrame>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QtGlobal>

namespace QUsbCanB {

class Worker : public QObject
{
    Q_OBJECT
public:
    explicit Worker(QObject *parent = nullptr);
    ~Worker() override;

    void setTxQueueLimit(int frames) { m_txQueueLimit = qMax(1, frames); }
    void setTxBatchSize(int frames) { m_txBatchSize = qBound(1, frames, 48); }

public slots:
    void start(int channel, quint32 bitrate, quint8 mode, int pollIntervalMs);
    void stop();
    void sendFrame(const QCanBusFrame &frame);

signals:
    void opened();
    void closed();
    void errorText(const QString &text);
    void framesReceived(const QList<QCanBusFrame> &frames);
    void framesWritten(qint64 count);
    void framesDropped(qint64 count);

private slots:
    void pollRx();
    void flushTx();

private:
    QList<QCanBusFrame> takeTxBatch();
    void requeueFront(const QList<QCanBusFrame> &frames, int first);

    static qusbcanb::Channel channelFromInt(int channel) noexcept;
    static qusbcanb::CanMode modeFromValue(quint8 mode) noexcept;
    static qusbcanb::CanFrame toLowLevelFrame(const QCanBusFrame &frame);
    static QCanBusFrame toQtFrame(const qusbcanb::CanFrame &frame);

    qusbcanb::LowLevelDevice m_device;
    QTimer *m_rxTimer = nullptr;
    QTimer *m_txTimer = nullptr;
    QQueue<QCanBusFrame> m_txQueue;
    int m_txQueueLimit = 1000;
    int m_txBatchSize = 48;
    int m_channel = 1;
    bool m_running = false;
};

} // namespace QUsbCanB
