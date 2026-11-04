# Support and sensitive reports

[English](SECURITY.md) · [简体中文](SECURITY.zh-CN.md)

Published stable versions are listed on the [Releases page](https://github.com/linker-bot/linkerbot-transport-cpp/releases/latest).
Maintenance is best-effort on the current `main` branch. The release does not imply
a separate long-term-support branch or a response-time guarantee. Historical
commits and unpublished CI artifacts have no separate support commitment.

## Ordinary project issues

Build failures, configuration mistakes, lifecycle bugs, process-exit crashes,
driver errors, and hardware compatibility gaps are normally reliability issues.
Report them with a fixed commit, trigger, prerequisites, required privileges,
actual impact, and the unaffected boundary. Do not describe them as an
authentication bypass or vulnerability unless a real security boundary is crossed.

Use the [canonical project's issue tracker](https://github.com/linker-bot/linkerbot-transport-cpp/issues)
for non-sensitive reports when it is enabled, or submit a pull request with a
minimal reproducer. Follow [CONTRIBUTING.md](CONTRIBUTING.md) and remove credentials,
verification codes, private logs, proprietary headers, vendor binaries, and material
that cannot be published.

## Sensitive reports

Do not put exploit details, credentials, or private system information in a public
issue or pull request. Follow the current
[Linkerbot organization security policy](https://github.com/linker-bot/.github/security/policy)
for its private contact instructions. This repository does not duplicate that
contact so changes remain centralized in the organization policy.

Include the repository and full commit, trigger and prerequisites, required
privileges, actual impact and boundary, a safely shareable reproducer, and any
temporary mitigation. No fixed response time is promised.

When physical devices are involved, stop testing if unexpected motion or unsafe
output occurs. Test only systems and devices you own or are authorized to use.
