# Python pre-integration evidence

This directory is an isolated proof fixture. It does not import or modify the
Python transport, hand or arm SDKs. It is not a supported binding or wheel.
The existing Python implementation remains the accepted business baseline.

## What is tested

1. A Python Future and native receipt slot are created and registered before
   sending. The native receipt moves into that slot without allocation. An actual
   CPython capsule allocation failure occurs before admission. An injected
   `bad_alloc` after admission and a Python result-wrapping failure leave the same
   Future/receipt discoverable; retrying result publication does not resend.
   `tests/lifecycle_allocation_failure.cpp` separately sweeps real native allocation
   failures in device/latest admission and fails a wrapper allocation after acceptance.
2. Native waits and close release the GIL. Independent completion threads demonstrate
   that an Event-blocked callback on one device does not block another device's
   completion or the native writer. Callback ordering, immediate registration after
   completion, reentry, exception handling, wrapper destruction and interpreter exit
   run in subprocesses with an external timeout and Python's debug allocator.
3. Two independent consumer probes import one extension. Separate Runtime creation
   cannot bypass its existing registrations. Factory and backend counters observe
   one connection, equal channel-instance IDs and at most one backend write in flight.
   Eight receipt credits bound completion metadata until publication/callback completion;
   retaining completed Futures does not retain those credits or connection ownership. The Python registry contains
   Futures/receipt handles, not frames or another TX scheduling queue.
4. `check_deployment.py` checks the consistency of recorded deployment evidence.
   Missing configuration, unknown backend/options, unsupported timestamp/frame
   requirements, changed revisions/topology, fake reports and tampered evidence fail.
   This command performs no device I/O and does not independently attest that a
   report is genuine or that hardware tests ran.

The probe clients are not the real hand/arm SDKs. Actual SDK runtime sharing and
business equivalence remain unproved until those SDKs are connected. No motor,
brake, calibration or motion command is sent by these tests.

## Scope and limits

The fixture requires conventional GIL-enabled CPython 3.10 or later. Subinterpreter
imports and free-threaded builds are rejected. The allocator injection additionally
requires one interpreter and one Python thread, restores the allocator immediately,
and is test-only. The CI matrix records the environments actually executed; a
successful local run does not imply success on every Python/platform combination.

The callback tests use finite or GIL-releasing callbacks. An arbitrary extension
callback can retain the process-wide GIL indefinitely; this fixture cannot promise
that other Python threads progress in that case. Exit-with-worker/callback tests
exercise an explicit cooperative shutdown protocol. They do not establish that
unbounded callbacks or an uninterruptible real driver can be forcibly terminated.

`CandidateBridge` is executable test scaffolding. Its pending/publishing metadata
retains ownership through allocation failure and callback reentry. It is not the
public compatibility layer, exception mapping, clock bridge or SDK implementation.

## Run

Build/install the native library, then consume that installation:

```sh
cmake -S . -B build/proof-native -DCMAKE_BUILD_TYPE=Release \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=OFF \
  -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=OFF
cmake --build build/proof-native --config Release --parallel 4
cmake --install build/proof-native --config Release --prefix "$PWD/build/proof-install"
cmake -S validation/python-preintegration -B build/proof-python \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/build/proof-install" \
  -DPython3_EXECUTABLE=/path/to/python
cmake --build build/proof-python --config Release --parallel 4
ctest --test-dir build/proof-python -C Release --output-on-failure
```

Development headers matching the selected interpreter are required. `evidence.json`
records individual subprocess results, interpreter/platform, module/source hashes
and the CI revision when available. Test reports distinguish actual allocation
injection from selected fault-point simulation; neither is an exhaustive proof
against permanent process-wide OOM or process termination.

## Deployment evidence

`baseline-inventory.json` records inspected source revisions and the user's
confirmation that Python has been accepted. Inspected revisions are not substituted
for unspecified deployed revisions. Historical C++ hand reports refer to their own
comm revisions and bounded environments, not the current Runtime or this probe.

`backend-contracts.json` is an API/source inventory. Native configuration validators
remain authoritative for legal values. In particular:

- `Message::timestamp` preserves native timestamp value, unit, clock and source.
  SocketCAN can request software or raw hardware timestamps; PCAN keeps driver
  microseconds; vendor keeps its raw 32-bit counter. No clock/epoch conversion or
  counter unwrapping is implied. These SLCAN profiles do not provide timestamps.
- `Message::format = FrameFormat::kClassic` explicitly sends classic CAN on FD
  links; `kUnspecified` preserves each backend's existing default.
- `receive_own_messages` is opt-in on SocketCAN and PCAN. The vendor and SLCAN
  interfaces reject opt-in rather than synthesize software echo frames.
- arbitrary python-can plugins/options are not implied by the available native backends;
- vendor `frame_type` is a retained, ignored legacy field, not an applied TX setting.

Populate `deployment.template.json` from the accepted deployment without guessing
missing values. Each channel must identify adapter/driver, devices and firmware,
resolved accepted-Python/native/external settings and timestamp/format dependencies.
Required features also need matching feature-specific native hardware checks.
Every `python_options` key needs an `option_mapping` entry with `kind` native/external
and its target `field`, backend selector equality, or the known false receive-own
default. Direct mappings must preserve JSON types and values. Unmapped options and
nontrivial conversions fail until a tested conversion contract exists. Evidence files
must be relative to the manifest and SHA-256 checked. Each record must identify
`kind: native-hardware`, `result: passed`, the exact native and Python revisions,
`configuration_sha256` from `configuration_digest(platform, channel)`, and checks
`identity`, `tx_rx`, `shared_lifecycle`, `recovery`, `shutdown`.

```sh
python validation/python-preintegration/check_deployment.py deployment.json \
  --report deployment-verdict.json
```

The supplied template intentionally fails. Synthetic records in `test_deployment.py`
only test the gate. Neither these fixtures nor historical/Python-only acceptance
records count as current native hardware acceptance. Hardware support stays
unverified until matching native evidence is supplied.

Reference contracts: [thread state and GIL](https://docs.python.org/3/c-api/init.html),
[capsule ownership](https://docs.python.org/3/c-api/capsule.html),
[allocator replacement](https://docs.python.org/3/c-api/memory.html).

Native API semantics and defaults are described in [CAN metadata](../../docs/en/can-metadata.md).
Backend capability declarations and synthetic metadata tests are not hardware acceptance.
