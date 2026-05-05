#pragma once

#include "qusbcanb_lowlevel.h"
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QQueue>
#include <QtCore/QTimer>
#include <QtSerialBus/QCanBusFrame>

namespace QUsbCanB {

class Worker : public QObject {
    Q_OBJECT
public:
    explicit Worker(QObject* parent = nullptr);
    ~Worker() override;

    void setTxQueueLimit(int frames) { m_txQueueLimit = qMax(1, frames); }
    void setTxBatchSize(int frames) { m_txBatchSize = qBound(1, frames, 48); }

public slots:
    void start(int channel, quint32 bitrate, quint8 mode, int pollIntervalMs);
    void stop();
    void sendFrame(const QCanBusFrame& frame);

signals:
    void opened();
    void closed();
    void errorText(const QString& text);
    void framesReceived(const QList<QCanBusFrame>& frames);
    void framesWritten(qint64 count);
    void framesDropped(qint64 count);

private slots:
    void pollRx();
    void flushTx();

private:
    QList<QCanBusFrame> takeTxBatch();
    void requeueFront(const QList<QCanBusFrame>& frames, int first);

    qusbcanb::LowLevelDevice m_device;
    QTimer* m_rxTimer = nullptr;
    QTimer* m_txTimer = nullptr;
    QQueue<QCanBusFrame> m_txQueue;
    int m_txQueueLimit = 1000;
    int m_txBatchSize = 48;
    int m_channel = 1;
    bool m_running = false;
};

} // namespace QUsbCanB
