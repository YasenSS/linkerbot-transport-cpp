# Transport 0.3.4 Stress Test Results

- [Stress test report](stress-report.md)
- [Raw data archive](stress-raw-data.tar.gz)

The test ran on November 10, 2026 against commit `0e190032d65ed0b315337b45b3779cf4f650ed68`. It covered 119 configurations, including six failed overload configurations. All four scenarios completed two consecutive 60-second lossless confirmations.

This English edition retains the condensed test-information section. The report, diagnostic descriptions and archived Python tool snapshot have been translated. All numeric measurements, CSV samples, logs, environment metadata, compiler records, C/C++ sources and compiled artifacts are unchanged. The archive includes `localization.json` with the original hashes and translation scope, and a regenerated `manifest.json`.

## Verify the English Archive

Use the translated verifier bundled in the archive. The current repository verifier renders Chinese reports and is intended for its original-format output, not this English edition. From this directory, run:

```sh
archive_path="$PWD/stress-raw-data.tar.gz"
verify_dir=$(mktemp -d)
tar -xzf "$archive_path" -C "$verify_dir" tool-source/run.py
PYTHONDONTWRITEBYTECODE=1 python3 "$verify_dir/tool-source/run.py" --verify "$archive_path"
rm -r -- "$verify_dir"
```

The verifier checks every archive checksum, per-configuration JSON records, validity assessments, socket counters, RSS summaries, the linked-library checksum, confirmation records and report regeneration. Verification does not run the stress test or open a hardware channel. Expected summary: 119 records, four confirmed scenarios, unchanged source, and no run-level errors. The six failed overload configurations remain recorded separately.
