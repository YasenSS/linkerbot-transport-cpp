# Package and target names

Starting with 0.3.3, `linkerbot-transport` names the CMake project, installed package
and generated SDK archives. Published releases through v0.3.2 retain their
original `linkerbot-comm` names and contents.

```cmake
find_package(linkerbot-transport 0.3 CONFIG REQUIRED COMPONENTS can)
target_link_libraries(app PRIVATE linkerbot::can)
```

Choose targets by module:

- `linkerbot::can`: CAN/CAN FD backends, SLCAN, buses, frame routing and CAN Session.
- `linkerbot::serial`: native serial byte streams, when enabled.
- `linkerbot::modbus`: Modbus RTU transactions and its serial dependency, when enabled.
- `linkerbot::headers`: include paths and C++17 requirements, without compiled libraries.
- `linkerbot::can-compat`: CAN linkage plus the legacy flat include paths.

CAN does not link serial or Modbus. Link each module used by the application.
Package components use these lowercase target names; an unavailable required
component makes discovery fail. Existing C++ namespaces and header paths do not
change, including `linkerbot::transport::Session` for CAN.

## Compatibility

Existing `find_package(linkerbot-comm 0.3 CONFIG REQUIRED)` calls remain supported.
The old `linkerbot::comm`, `linkerbot::comm-values` and `linkerbot::comm-compat`
targets forward to `can`, `headers` and `can-compat`. They retain the original
CAN-only dependency scope. Source-tree builds also provide the old unqualified
library aliases. Aliases are link targets, not independent command-line build
targets; use `cmake --build build --target linkerbot-can` for the compiled CAN library.

The canonical CAN archive is `liblinkerbot-can.a` on Unix or `linkerbot-can.lib` on
Windows. A byte-identical `linkerbot-comm` archive is included for applications
that link by file name. Do not link both copies into an application or depend on
them as separate implementations. New SDK filenames begin with
`linkerbot-transport-`; existing published download URLs are unchanged.

Documentation, examples and `build-info.json` are installed under
`share/linkerbot-transport`. The former `share/linkerbot-comm` tree contains the
same files. Both CMake package directories remain installed and relocatable.

New cache options and exported provenance properties use `LINKERBOT_TRANSPORT_`.
The corresponding `LINKERBOT_COMM_` names remain supported. Existing cache values
initialize the new names; changing either spelling in a previously configured
tree updates its partner. Conflicting initial values, or conflicting edits to
both spellings, fail configuration. `LINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS` and
`LINKERBOT_PERF_COUNT_ALLOCATIONS` keep their names.

Metadata keeps schema version 1 with additive fields. New packages identify
`package` as `linkerbot-transport`, list `can` in `components`, and record
`transport_sha`. The retained `comm_sha` has the same value;
`compatibility_libraries: ["comm"]` declares the duplicate archive. Validators
check byte identity and continue to accept historical comm packages.
The `LINKERBOT_COMM_SLCAN_FD` feature macro remains available alongside
`LINKERBOT_TRANSPORT_SLCAN_FD`.

## Upgrade and rollback

Use separate build and install directories for the migration. Build the SDK,
run canonical and legacy consumer checks, then rebuild dependent applications.
The old package spelling works with both old and new SDKs. Applications adopting
the canonical package name require the new SDK; reverting only the SDK will not
make that new CMake call work with older packages. Keep the matching application
build configuration when rolling back. Archive filename-based download scripts
must adopt the new prefix for future releases.

This migration does not change the hand SDK, publish packages or replace release
assets. It preserves the existing ABI and CAN/serial/RTU behavior; offline package
and consumer validation does not establish hardware acceptance.
