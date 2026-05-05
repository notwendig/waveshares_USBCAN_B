# QUsbCanB POSIX/libusb LowLevel v10

Qt-free low-level backend for Waveshare USBCANB / CANalyst-II compatible adapters.

## v10 fix

v9 proved the real CANalyst-II USB message protocol works: `--count 1` produced cross-channel RX.
However, older Qt wrapper code often calls `readFrames(channel, 1)`. One USB RX packet can contain up to 3 CAN messages. v9 decoded all messages but returned only one and discarded the rest when `maxFrames == 1`, causing RX counts around one third of TX counts.

v10 adds a small per-channel RX cache inside the Qt-free backend. Extra decoded frames are preserved and returned by later `readFrames()` calls.

Expected with CAN1 connected to CAN2:

```text
./qusbcanb_regression --count 1
TX1=1/1 TX2=1/1 RX1=1 RX2=1

./qusbcanb_regression --count 100
TX1=100/100 TX2=100/100 RX1≈100 RX2≈100
```

If RX is still lower than TX at high counts, the regression test may need to keep polling RX until the expected counts arrive or a timeout expires.
