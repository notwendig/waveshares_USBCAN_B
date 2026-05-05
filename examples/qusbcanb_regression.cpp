#include "qusbcanb_lowlevel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCommandLineParser>
#include <QtSerialBus/QCanBusFrame>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

using namespace QUsbCanB;

static QByteArray makePayload(int channel, int seq)
{
    QByteArray p;
    p.append(char(0x51)); // Q
    p.append(char(0x55)); // U
    p.append(char(0x42)); // B
    p.append(char(channel & 0xff));
    p.append(char(seq & 0xff));
    p.append(char((seq >> 8) & 0xff));
    p.append(char((seq >> 16) & 0xff));
    p.append(char((seq >> 24) & 0xff));
    return p;
}

static QCanBusFrame makeFrame(int channel, int seq)
{
    QCanBusFrame f(channel == 1 ? 0x121u : 0x122u, makePayload(channel, seq));
    f.setExtendedFrameFormat(false);
    return f;
}

class BlockingFrameQueue {
public:
    explicit BlockingFrameQueue(int maxItems) : m_maxItems(maxItems) {}

    bool push(QCanBusFrame frame)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notFull.wait(lock, [&] { return m_done || int(m_queue.size()) < m_maxItems; });
        if (m_done)
            return false;
        m_queue.push_back(std::move(frame));
        m_notEmpty.notify_one();
        return true;
    }

    QList<QCanBusFrame> waitTakeUpTo(int n, bool producerDone)
    {
        QList<QCanBusFrame> out;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait(lock, [&] { return m_done || !m_queue.empty() || producerDone; });
        if (m_queue.empty())
            return out;
        const int count = std::min<int>(n, int(m_queue.size()));
        out.reserve(count);
        for (int i = 0; i < count; ++i) {
            out.append(m_queue.front());
            m_queue.pop_front();
        }
        m_notFull.notify_all();
        return out;
    }

    void requeueFront(const QList<QCanBusFrame>& frames, int first = 0)
    {
        if (first >= frames.size())
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int i = frames.size() - 1; i >= first; --i)
            m_queue.push_front(frames.at(i));
        m_notEmpty.notify_one();
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    int size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return int(m_queue.size());
    }

    void wakeAll()
    {
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

private:
    const int m_maxItems;
    mutable std::mutex m_mutex;
    std::condition_variable m_notFull;
    std::condition_variable m_notEmpty;
    std::deque<QCanBusFrame> m_queue;
    bool m_done = false;
};

static int drainRx(LowLevelDevice& dev, int channel, int rounds, int timeoutMs, std::atomic<int>& counter)
{
    int got = 0;
    for (int i = 0; i < rounds; ++i) {
        QString err;
        const auto frames = dev.readFrames(channel, timeoutMs, &err);
        if (frames.isEmpty())
            break;
        got += frames.size();
        counter.fetch_add(frames.size());
    }
    return got;
}

struct ChannelState {
    BlockingFrameQueue queue;
    std::atomic<int> produced{0};
    std::atomic<int> tx{0};
    std::atomic<int> rx{0};
    std::atomic<int> dropped{0};
    std::atomic<int> shortAccepts{0};
    std::atomic<int> timeouts{0};
    std::atomic<int> errors{0};
    std::atomic<bool> producerDone{false};
    int adaptiveBatch = 48;
    int goodBursts = 0;

    explicit ChannelState(int queueLimit) : queue(queueLimit) {}
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("QUsbCanB v19 regression: adaptive 1000-frame TX queues, sleeping TX worker, up to 48-frame batches");
    parser.addHelpOption();
    QCommandLineOption countOpt(QStringLiteral("count"), QStringLiteral("Frames generated per channel."), QStringLiteral("n"), QStringLiteral("10000"));
    QCommandLineOption batchOpt(QStringLiteral("batch"), QStringLiteral("Initial/max CAN frames per channel batch, max/recommended 48."), QStringLiteral("n"), QStringLiteral("48"));
    QCommandLineOption queueOpt(QStringLiteral("queue"), QStringLiteral("TX queue size per channel, max/recommended 1000."), QStringLiteral("n"), QStringLiteral("1000"));
    QCommandLineOption delayOpt(QStringLiteral("batch-delay-ms"), QStringLiteral("Base delay after each dual-channel cycle."), QStringLiteral("ms"), QStringLiteral("5"));
    QCommandLineOption backoffOpt(QStringLiteral("backoff-ms"), QStringLiteral("Backoff after USB timeout/backpressure."), QStringLiteral("ms"), QStringLiteral("50"));
    QCommandLineOption rxRoundsOpt(QStringLiteral("rx-rounds"), QStringLiteral("RX drain reads per channel after each cycle."), QStringLiteral("n"), QStringLiteral("16"));
    QCommandLineOption rxTimeoutOpt(QStringLiteral("rx-timeout-ms"), QStringLiteral("RX timeout for each drain read."), QStringLiteral("ms"), QStringLiteral("5"));
    QCommandLineOption selfTestOpt(QStringLiteral("self-test"), QStringLiteral("Use ControlCAN self-test mode 2."));
    parser.addOption(countOpt);
    parser.addOption(batchOpt);
    parser.addOption(queueOpt);
    parser.addOption(delayOpt);
    parser.addOption(backoffOpt);
    parser.addOption(rxRoundsOpt);
    parser.addOption(rxTimeoutOpt);
    parser.addOption(selfTestOpt);
    parser.process(app);

    const int count = qMax(0, parser.value(countOpt).toInt());
    const int maxBatch = qBound(1, parser.value(batchOpt).toInt(), 48);
    const int queueLimit = qBound(1, parser.value(queueOpt).toInt(), 1000);
    const int baseDelayMs = qMax(0, parser.value(delayOpt).toInt());
    const int backoffMs = qMax(1, parser.value(backoffOpt).toInt());
    const int rxRounds = qMax(0, parser.value(rxRoundsOpt).toInt());
    const int rxTimeoutMs = qMax(0, parser.value(rxTimeoutOpt).toInt());
    const quint8 mode = parser.isSet(selfTestOpt) ? 2 : 0;

    std::cout << "QUsbCanB v19 regression: count=" << count
              << " per channel, mode=" << (mode == 2 ? "self-test" : "normal")
              << ", queue=" << queueLimit
              << ", initial/max-batch=" << maxBatch
              << ", base-delay-ms=" << baseDelayMs
              << ", backoff-ms=" << backoffMs
              << ", producer threads=2, TX worker threads=1\n";
    std::cout << "CAN1: CMD 0x02 TX 0x01 RX 0x81\n";
    std::cout << "CAN2: CMD 0x04 TX 0x03 RX 0x83\n";

    LowLevelDevice dev;
    QString err;
    if (!dev.open(&err)) {
        std::cerr << "open failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!dev.configureBothAndStart(125000, mode, &err)) {
        std::cerr << "configureBothAndStart failed: " << err.toStdString() << "\n";
        return 2;
    }

    ChannelState c1(queueLimit);
    ChannelState c2(queueLimit);
    c1.adaptiveBatch = maxBatch;
    c2.adaptiveBatch = maxBatch;
    std::atomic<bool> stop{false};

    auto producer = [&](int channel, ChannelState& c) {
        for (int i = 0; i < count && !stop.load(); ++i) {
            if (!c.queue.push(makeFrame(channel, i)))
                break;
            c.produced.fetch_add(1);
        }
        c.producerDone = true;
        c.queue.wakeAll();
    };

    std::thread p1(producer, 1, std::ref(c1));
    std::thread p2(producer, 2, std::ref(c2));

    auto transmitOne = [&](int channel, ChannelState& c) -> bool {
        const QList<QCanBusFrame> batch = c.queue.waitTakeUpTo(c.adaptiveBatch, c.producerDone.load());
        if (batch.isEmpty())
            return false;

        QString e;
        const int sent = dev.writeFrames(channel, batch, &e);
        if (sent > 0)
            c.tx.fetch_add(sent);

        if (sent == batch.size()) {
            c.goodBursts++;
            if (c.goodBursts >= 8 && c.adaptiveBatch < maxBatch) {
                c.adaptiveBatch = qMin(maxBatch, c.adaptiveBatch + 3);
                c.goodBursts = 0;
            }
            return true;
        }

        c.goodBursts = 0;
        const bool timeout = e.contains(QStringLiteral("LIBUSB_ERROR_TIMEOUT"));
        const bool noDevice = e.contains(QStringLiteral("LIBUSB_ERROR_NO_DEVICE"));

        if (sent > 0 && sent < batch.size()) {
            c.shortAccepts.fetch_add(1);
            c.queue.requeueFront(batch, sent);
            c.adaptiveBatch = qBound(1, sent, maxBatch);
            c.timeouts.fetch_add(timeout ? 1 : 0);
            return true;
        }

        if (timeout) {
            // Firmware TX FIFO/backpressure: no frame accepted. Keep data, sleep, try later.
            c.timeouts.fetch_add(1);
            c.queue.requeueFront(batch, 0);
            c.adaptiveBatch = qMax(1, c.adaptiveBatch / 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            return true;
        }

        // Hard USB-level error before adapter accepted any frame: log & forget this batch.
        c.errors.fetch_add(1);
        c.dropped.fetch_add(batch.size());
        if (!e.isEmpty())
            std::cerr << "CAN" << channel << " dropped " << batch.size()
                      << " frames after hard USB error: " << e.toStdString() << "\n";
        if (noDevice)
            stop = true;
        return true;
    };

    auto worker = [&] {
        int lastReport = 0;
        while (!stop.load()) {
            const bool done1 = c1.producerDone.load() && c1.queue.empty();
            const bool done2 = c2.producerDone.load() && c2.queue.empty();
            if (done1 && done2)
                break;

            const bool did1 = transmitOne(1, c1);
            const bool did2 = transmitOne(2, c2);

            if (rxRounds > 0) {
                drainRx(dev, 1, rxRounds, rxTimeoutMs, c1.rx);
                drainRx(dev, 2, rxRounds, rxTimeoutMs, c2.rx);
            }

            const int totalDone = c1.tx.load() + c2.tx.load() + c1.dropped.load() + c2.dropped.load();
            if (totalDone / 1000 > lastReport) {
                lastReport = totalDone / 1000;
                std::cout << "progress: TX1=" << c1.tx.load() << " TX2=" << c2.tx.load()
                          << " RX1=" << c1.rx.load() << " RX2=" << c2.rx.load()
                          << " q1=" << c1.queue.size() << " q2=" << c2.queue.size()
                          << " b1=" << c1.adaptiveBatch << " b2=" << c2.adaptiveBatch
                          << " short1=" << c1.shortAccepts.load() << " short2=" << c2.shortAccepts.load()
                          << " timeout1=" << c1.timeouts.load() << " timeout2=" << c2.timeouts.load()
                          << "\n";
            }

            if (baseDelayMs > 0 || (!did1 && !did2))
                std::this_thread::sleep_for(std::chrono::milliseconds(qMax(1, baseDelayMs)));
        }
    };

    std::thread tx(worker);

    p1.join();
    p2.join();
    c1.queue.wakeAll();
    c2.queue.wakeAll();
    tx.join();

    for (int i = 0; i < 100; ++i) {
        const int got = drainRx(dev, 1, rxRounds, rxTimeoutMs, c1.rx)
                      + drainRx(dev, 2, rxRounds, rxTimeoutMs, c2.rx);
        if (got == 0)
            break;
    }

    std::cout << "summary: produced1=" << c1.produced.load()
              << " produced2=" << c2.produced.load()
              << " TX1=" << c1.tx.load() << "/" << count
              << " TX2=" << c2.tx.load() << "/" << count
              << " RX1=" << c1.rx.load()
              << " RX2=" << c2.rx.load()
              << " drop1=" << c1.dropped.load()
              << " drop2=" << c2.dropped.load()
              << " short1=" << c1.shortAccepts.load()
              << " short2=" << c2.shortAccepts.load()
              << " timeout1=" << c1.timeouts.load()
              << " timeout2=" << c2.timeouts.load()
              << " err1=" << c1.errors.load()
              << " err2=" << c2.errors.load() << "\n";

    return stop.load() ? 4 : 0;
}
