# CAN frame selection and receive metadata

The additions in this change preserve source-level calls and existing aggregate
initializers. Rebuild consumers with the matching headers and library; changing
`Message` and configuration sizes does not preserve a precompiled binary ABI.
No hand/arm SDK or Python production binding is changed.

## Explicit classic frames on an FD link

Set `message.format = FrameFormat::kClassic` to send a classic frame on an FD
channel. It requires at most eight bytes and rejects FD/BRS flags. Set
`FrameFormat::kFd` for FD; a classic-only channel rejects it. Existing BRS rules
still apply: `is_brs` requires `is_fd`. Unknown format values are rejected.

`FrameFormat::kUnspecified` is the default. It retains the old SocketCAN/vendor
behavior (untagged frames on FD links become FD) and PCAN/SLCAN behavior (untagged
frames stay classic). Existing `is_fd`/`is_brs` callers need no changes. Received
frames keep the existing flags and leave the new TX override unspecified. A
forwarder that needs classic-on-FD must explicitly choose the classic override.
`Session::submit` validates the override before admission or latest replacement.
Receive route keys use the arbitration ID and standard/extended-ID flag, not this
TX classic/FD override.

## Receive timestamps

`Message::timestamp` is optional. Its value, unit, clock, source and counter width
travel with the message through Session routes; they do not replace
`RoutedMessage::received_at`, which remains a local steady-clock observation.
TX ignores receive metadata. Absence means no timestamp was provided; it is not
replaced with a host read time.

- SocketCAN: `SocketCANOptions::timestamp_mode` defaults to `kDisabled`.
  `kSoftware` requests kernel software timestamps (system realtime nanoseconds).
  `kHardware` requests raw hardware timestamps via `SO_TIMESTAMPING`; these use
  the device clock. Hardware mode does not request software fallback. Interface
  hardware timestamp configuration, adapter/driver support and clock correlation
  are deployment responsibilities. Socket-option success does not prove the
  driver supplies hardware timestamps. Missing, truncated or invalid metadata
  stays absent. A synthetic ancillary message is not hardware evidence.
- PCAN: preserve driver microseconds without floating point or epoch conversion.
  Classic timestamps include `millis + (overflow << 32)` before conversion to
  microseconds; FD timestamps keep all 64 bits. Clock and source are `kDriver`: the API does
  not provide a per-frame hardware/software provenance flag. Do not infer a UTC
  epoch or hardware precision from the value alone.
- Vendor CAN FD: preserve `CanFDMsg::TimeStamp`, including zero and wraparound,
  as 32-bit `kRawTicks` with `kVendorRaw` clock and `kDriver` source. The exposed
  ABI does not establish a portable unit or epoch. Python's ctypes backend also
  returns this raw integer. No wrap extension or fabricated conversion is used.
- SLCAN: existing firmware profiles do not expose receive timestamps. No timestamp
  is synthesized and no startup commands/defaults are changed.

## Receive own messages

SocketCAN's `SocketCANOptions::receive_own_messages` and PCAN's
`PcanConfig::receive_own_messages` default to false. Opt-in configures
`CAN_RAW_RECV_OWN_MSGS` or `PCAN_ALLOW_ECHO_FRAMES` (PCAN-Basic 4.6 or later,
subject to driver support). This uses the existing reader and native send path;
there is no second TX queue or software echo queue.

`Message::is_rx` is optional. SocketCAN uses `MSG_CONFIRM` and PCAN uses the echo
flag to report own messages as false; ordinary received frames are true. Vendor
and SLCAN leave direction unknown because their exposed interfaces do not prove
that distinction. Their added `receive_own_messages` configuration accepts the
existing false default and rejects true before native initialization. No hidden
local echo is promised for these backends.

The dynamically loaded PCAN setter is optional for compatibility with old
libraries and injected API tables. Enabling echo without it fails before channel
initialization. A driver setter failure is reported and initialized resources
are released or retained for the existing cleanup retry protocol.

Include all selected options in a shared Runtime's configuration fingerprint.
The deployment evidence gate requires backend-specific capability, enabled
settings and feature-specific hardware checks. Native hardware timestamp origin,
clock behavior, actual echo delivery and mixed classic/FD bus traffic remain
**pending physical validation** for each deployed adapter/driver/firmware set.

References: [Linux timestamping](https://www.kernel.org/doc/html/latest/networking/timestamping.html),
[SocketCAN](https://www.kernel.org/doc/html/latest/networking/can.html),
[PCAN parameter documentation](https://www.peak-system.com/produktcd/Develop/PC%20interfaces/Windows/PCAN-Basic%20API/PCAN-Parameter_Documentation.pdf).
