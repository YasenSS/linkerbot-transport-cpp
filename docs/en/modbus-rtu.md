# Serial byte streams and Modbus RTU

These optional modules provide Linux/Windows serial I/O and a Modbus RTU client.
They do not implement hand register maps, model discovery, motion control, or
Modbus TCP/ASCII. Existing CAN and SLCAN interfaces are unchanged. A serial SLCAN
adapter still speaks its configured CAN protocol; it is not an RTU endpoint.

## Build and consume

`LINKERBOT_COMM_ENABLE_SERIAL` and `LINKERBOT_COMM_ENABLE_MODBUS` default to `ON`.
Modbus requires serial. Disable both for a CAN-only installation. Neither module
is linked by `linkerbot::comm`; consumers opt in through separate exported targets.

```cmake
find_package(linkerbot-comm 0.3 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE linkerbot::modbus) # includes linkerbot::serial
```

```cpp
#include <linkerbot/modbus/rtu.hpp>

linkerbot::modbus::RtuConfig config;
config.serial.port = "/dev/ttyUSB0"; // Windows: COM3
config.serial.baud_rate = 115200;
config.serial.parity = linkerbot::serial::Parity::kEven;
config.serial.stop_bits = linkerbot::serial::StopBits::kOne;
auto session = linkerbot::modbus::RtuSession::create(config);
linkerbot::modbus::Client client(session, 1);
const auto values = client.read_holding_registers(0, 2,
    {linkerbot::transport::Clock::now() + std::chrono::milliseconds{100}, {}});
session->close();
```

This illustrates API composition, not a validated hand request. Select the actual
serial format, unit ID, register range and timeout from the device specification.
Constructing a native session opens exactly the configured port; there is no scan.
Native serial is supported on Linux and Windows, not macOS. The injected
`ByteStream` interface and RTU codec/session remain portable.

## Serial contract

`serial::open_serial` returns an open, exclusive stream. Configuration supports
5--8 data bits, none/even/odd parity and one/two stop bits. RTU requires eight data
bits. The default is 115200, 8E1; select 8N2 or a device-specific 8N1 explicitly.
Kernel-managed RS485 direction is Linux-only and fails if the driver does not
support it. Windows requires an automatic-direction adapter; software RTS
bit-banging is not implemented. Hardware configuration needs adapter validation.

`read_some` returns zero on its deadline, throws on EOF/I/O failure or
cancellation, and can return a prefix. `write_some` can also return a prefix;
a later error does not undo those bytes. `drain_output_until` observes the driver
output queue, not a peer acknowledgement or a certified USB/wire timestamp.
Operations share absolute steady-clock deadlines, including same-direction lock
contention. Native waits check cancellation at bounded polling intervals.
`cancel()` permanently interrupts this stream. `close()` is idempotent and waits
for active operations to return. Keep the stream alive through concurrent calls.

Windows pending OVERLAPPED operations must finish cancellation before their
buffers can be destroyed. A defective driver may delay that cleanup beyond the
requested deadline. Do not treat this API as a hard real-time driver boundary.

## RTU contract

Supported functions are 03/04 (read holding/input registers), 06 (write one) and
10 (write multiple). Unit IDs are 1--247; broadcasts are rejected. Addresses are
zero-based PDU addresses. Registers use big-endian bytes, CRC low byte precedes
high byte. Requests validate protocol count/address limits; ADUs are bounded at
256 bytes and lengths use `size_t`.

Share one `RtuSession` between all units on a physical line. It has a bounded FIFO
and only one active request/reply exchange, including output drain. Do not create
one native session per unit on the same port. Linux uses flock and TIOCEXCL;
Windows uses exclusive CreateFile. Occupied SLCAN ports cannot be reused as RTU.

A transaction requires a finite deadline covering admission, the idle interval,
partial writes, output drain and response. Invalid requests, unavailable sessions,
queue overflow, cancellation and timeout before admission throw. Once admitted,
`TransactionResult` retains failures in `error`; call `rethrow_if_error()` or
inspect `outcome`. Typed reads rethrow failures. Typed writes return the receipt,
including `bytes_sent`, `request_dispatched`, `delivery_uncertain` and generation.
Zero known sent bytes does not imply zero delivery if a backend write threw.

A successful write response verifies only the protocol echo; it proves neither
mechanical arrival nor configuration readback. Device exceptions retain their
code and do not fault a synchronized line. Other uncertain post-send failures
fault the session. No operation is retried automatically, including reads:
application-level read side effects are outside the generic layer's knowledge.

RTU has no transaction ID. A late reply may be indistinguishable from a response
to the next identical request. New requests on a faulted line are rejected.
`recover(observed_generation)` cancels active work, rejects queued work, closes
and recreates the stream, and increments the shared generation without replay.
The caller must establish that old device responses cannot still arrive (for
example, after a documented response bound or device reset). Reopening alone
cannot establish that fact. The next transaction rejects pre-existing input and
requires a quiet interval. Recovery is not a device identity check. Upper layers
must invalidate caches and reverify every device sharing the line. Explicit close
prevents further recovery. Call close/recover outside backend callbacks.

At up to 19200 baud, gaps derive from the configured character format; higher
rates use 750 us and 1750 us. USB drivers may aggregate bytes and do not expose
accurate per-byte timing. Response length, CRC, identifiers, byte counts and write
echoes are checked independently. Tests of a fake stream or PTY do not certify
physical inter-character timing or RS485 turnaround.

Local echo removal is disabled by default. Set `discard_local_echo` only when the
adapter is known to echo every request. The complete echo must match the request;
without correct configuration a 06 echo is indistinguishable from a device reply.

## Validation

The test suites include codec vectors, fragmented streams, exceptions, partial
writes, queue contention, cancellation, stale input and shared recovery. Linux PTY
tests cover native binary I/O, deadlines, exclusivity and disconnect. Windows
validation without an adapter covers build/configuration paths, not hardware I/O.
There is no hardware acceptance claim for either platform in this change.

```bash
cmake -S . -B build -DLINKERBOT_COMM_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --no-tests=error
```

The optional Clang fuzz target instruments the codec itself and is not installed:

```bash
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ \
  -DLINKERBOT_COMM_BUILD_TESTS=OFF -DLINKERBOT_COMM_BUILD_FUZZERS=ON
cmake --build build-fuzz --target rtu-codec-fuzz
./build-fuzz/rtu-codec-fuzz -runs=100000 -max_len=256
```

References: [Modbus serial line V1.02](https://www.modbus.org/docs/Modbus_over_serial_line_V1_02.pdf)
and [application protocol specifications](https://www.modbus.org/modbus-specifications).
