#pragma once

#include "qusbcanb_worker.h"
#include <QtCore/QThread>
#include <QtCore/QMutex>
#include <QtSerialBus/QCanBusDevice>

namespace QUsbCanB {

class QUsbCanBusDevice : public QCanBusDevice {
    Q_OBJECT
public:
    explicit QUsbCanBusDevice(int channel = 1, QObject* parent = nullptr);
    ~QUsbCanBusDevice() override;

    void setChannel(int channel);
    int channel() const { return m_channel; }

    void setPollInterval(int ms) { m_pollIntervalMs = qMax(1, ms); }
    int pollInterval() const { return m_pollIntervalMs; }

    bool writeFrame(const QCanBusFrame& frame) override;
    QString interpretErrorFrame(const QCanBusFrame& errorFrame) override;

protected:
    bool open() override;
    void close() override;

private slots:
    void onOpened();
    void onClosed();
    void onError(const QString& text);
    void onFramesReceived(const QList<QCanBusFrame>& frames);
    void onFramesWritten(qint64 count);

private:
    quint32 bitrateFromConfiguration() const;
    quint8 modeFromConfiguration() const;

    int m_channel = 1;
    int m_pollIntervalMs = 2;
    QThread m_thread;
    mutable QMutex m_apiMutex;
    Worker* m_worker = nullptr;
};

} // namespace QUsbCanB
