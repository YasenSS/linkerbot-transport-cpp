#!/usr/bin/env python3
"""Select bounded analyzer concurrency and preserve an auditable A/B input record."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, choices=(2, 4), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--config', type=Path, default=Path('.cppreportcard.json'))
    args = parser.parse_args()
    cpus = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else os.cpu_count()
    effective = min(args.jobs, cpus or 1)
    raw = args.config.read_bytes()
    config = json.loads(raw)
    config['jobs'] = effective
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'config.json').write_text(json.dumps(config, indent=2) + '\n')
    info = {'requested_jobs': args.jobs, 'effective_jobs': effective, 'available_cpus': cpus,
            'architecture': platform.machine(), 'source_sha': os.environ.get('GITHUB_SHA', 'unknown'),
            'run_id': os.environ.get('GITHUB_RUN_ID', 'local'),
            'run_attempt': os.environ.get('GITHUB_RUN_ATTEMPT', 'local'),
            'source_config_sha256': hashlib.sha256(raw).hexdigest()}
    (args.output / 'concurrency.json').write_text(json.dumps(info, indent=2) + '\n')
    if os.environ.get('GITHUB_ENV'):
        with open(os.environ['GITHUB_ENV'], 'a') as stream:
            stream.write('ANALYSIS_JOBS=%s\n' % effective)
    print(json.dumps(info))


if __name__ == '__main__':
    main()
