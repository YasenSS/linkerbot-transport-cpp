## Summary

## Reproduction

- Affected commit or version:
- Trigger and prerequisites:
- Required privileges:
- Actual impact and boundary:
- Result before the fix:

## Fix

- Root cause:
- Why validation and idempotency remain intact:

## Compatibility

- API, ABI, and source compatibility:
- Configuration and default changes:
- Persistent or old-record handling:
- Upgrade order:
- Rollback limits:

## Validation

### Local ARM64

- [ ] clang-format
- [ ] clang-tidy
- [ ] libc++ with warnings as errors
- [ ] CTest
- [ ] ASan and UBSan where applicable
- [ ] TSan where applicable
- [ ] Production line coverage at least 95%

Passed:

Failed:

Skipped, with reasons:

### GitHub CI

- Exact head:
- amd64:
- ARM64:
- Windows:
- Review and unresolved threads:

## Documentation

- Updated files:
- Links, defaults, and commands checked:

## Checklist

- [ ] Commits are signed.
- [ ] The regression fails before the fix and passes after it.
- [ ] Normal behavior and compatibility are covered.
- [ ] Commands and logs do not disclose credentials or verification codes.
- [ ] CI and review results refer to the latest PR head.
