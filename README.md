# QUsbCanB

Qt 6 / libusb driver library for the Waveshare USB-CAN-B / CANalyst-II compatible adapter `04d8:0053`.

## Confirmed endpoint map

```text
CAN1 CMD OUT 0x02, TX OUT 0x01, RX IN 0x81
CAN2 CMD OUT 0x04, TX OUT 0x03, RX IN 0x83
```

## Build

```bash
./build.sh
```

## Tests

```bash
./build/qusbcanb_test --init-only
./build/qusbcanb_test --self-test --count 10
./build/qusbcanb_test --channel 2 --init-only
```

## Regression: competing threads

Sends 10,000 CAN frames per channel while two RX threads poll both channels concurrently:

```bash
./build/qusbcanb_regression --self-test --count 10000
```

For a real crossed CAN1↔CAN2 bus at 125 kbit/s with termination/ACK:

```bash
./build/qusbcanb_regression --count 10000
```
