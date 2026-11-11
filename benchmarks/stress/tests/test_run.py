"""Failure evidence, process cleanup and report/archive integrity regressions."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run


def record():
    return dict(name='trial', backend='vcan', fd=False, phase='confirm', requested_seconds=60,
                threads=2, batch=8, window=8, buffer='large', peer=None,
                exit_code=0, timed_out=False,
                result=dict(**{k: 100 for k in ['sent', 'received', 'tx_frames', 'rx_frames', 'peer_received', 'peer_sent']},
                            **{k: 0 for k in ['overflows', 'route_misses', 'invalid_frames', 'inbox_pending']},
                            **{'pass': True}, frames_per_second=100/60, seconds=60,
                            roundtrip_p99_us_upper=99, cpu_percent_one_core=100, rss_final_kib=4096),
                probes=[dict(event=event, role=role, rcvbuf=8388608, drops=0, tune_errno=0, probe_errno=0)
                        for role in ['session', 'peer'] for event in ['open', 'close']],
                rss_samples_kib=dict(min=4096, max=4096, first=4096, last=4096))


class EvidenceTests(unittest.TestCase):
    def test_default_output_is_unique_and_separates_quick_results(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(run, 'RESULTS', Path(temp)):
            first = run.prepare_output(None, False)
            second = run.prepare_output(None, False)
            quick = run.prepare_output(None, True)
            self.assertNotEqual(first, second)
            self.assertTrue(first.name.startswith('stress-'))
            self.assertTrue(quick.name.startswith('quick-'))
            explicit = Path(temp) / 'custom'
            self.assertEqual(run.prepare_output(explicit, True), explicit.resolve())

    def test_archive_includes_nested_worker_and_test_sources(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            raw, out = root / 'raw', root / 'out'
            raw.mkdir(); out.mkdir()
            meta = dict(quick=True, errors=[], host={})
            for name, data in [('metadata.json', meta), ('runs.json', []), ('confirmed.json', {})]:
                run.save(raw / name, data)
            run.publish(raw, out, True, meta, [], {})
            with run.tarfile.open(out / '快速验证原始数据.tar.gz') as archive:
                for name in ['src/throughput.cpp', 'src/profile.cpp', 'src/socket_probe.c', 'tests/test_run.py']:
                    self.assertEqual(archive.extractfile('tool-source/' + name).read(), (run.HERE / name).read_bytes())

    def test_worker_success_cannot_hide_kernel_loss_or_missing_counter(self):
        good = record()
        self.assertEqual(run.assess(good), [])
        bad = copy.deepcopy(good)
        bad['probes'][1]['drops'] = 1
        self.assertTrue(any('内核丢帧' in e for e in run.assess(bad)))
        bad = copy.deepcopy(good)
        bad['probes'].pop()
        self.assertTrue(any('计数缺失' in e for e in run.assess(bad)))
        bad = copy.deepcopy(good)
        bad['result']['received'] -= 1
        self.assertIn('完整收发计数不一致', run.assess(bad))

    def test_timeout_exit_and_malformed_json_cannot_be_success(self):
        for field, value in [('timed_out', True), ('exit_code', 1), ('result', None)]:
            r = record()
            r[field] = value
            self.assertTrue(run.assess(r))

    def test_source_dirty_content_and_reverted_commit_detection(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            subprocess.run(['git', 'init', '-q', temp], check=True)
            subprocess.run(['git', '-C', temp, 'config', 'user.name', 'test'], check=True)
            subprocess.run(['git', '-C', temp, 'config', 'user.email', 'test@example.invalid'], check=True)
            f = p / 'code.cpp'
            f.write_text('before')
            subprocess.run(['git', '-C', temp, 'add', '.'], check=True)
            subprocess.run(['git', '-C', temp, 'commit', '-qm', 'fixture'], check=True)
            a = run.snapshot(p)
            f.write_text('after')
            self.assertFalse(run.same_source(a, run.snapshot(p)))
            f.write_text('before')
            self.assertTrue(run.same_source(a, run.snapshot(p)))
            (p / 'new.hpp').write_text('new')
            self.assertFalse(run.same_source(a, run.snapshot(p)))

    def test_timeout_kills_descendant_before_it_can_write(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            child = 'import time,pathlib;time.sleep(1);pathlib.Path(' + repr(str(p / 'orphan')) + ').touch()'
            parent = 'import subprocess,sys,time;subprocess.Popen([sys.executable,"-c",' + repr(child) + ']);time.sleep(10)'
            _, timed_out = run.execute([sys.executable, '-c', parent], p / 'out', p / 'err', .2)
            self.assertTrue(timed_out)
            time.sleep(1.1)
            self.assertFalse((p / 'orphan').exists())

    def test_report_and_archive_tampering_and_incomplete_results(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            raw, out = root / 'raw', root / 'out'
            raw.mkdir(); out.mkdir()
            meta = dict(quick=False, started_at='start', finished_at='end', source_unchanged=True,
                        errors=['injected failure'], host={})
            run.save(raw / 'metadata.json', meta)
            run.save(raw / 'runs.json', [])
            run.save(raw / 'confirmed.json', {})
            run.publish(raw, out, False, meta, [], {})
            report = (out / '压测报告.md').read_text()
            self.assertIn('流程未完成或测试无效', report)
            self.assertNotIn('四项无损确认有效', report)
            self.assertEqual(run.verify_archive(out / '压测原始数据.tar.gz')['confirmed'], 0)
            with self.assertRaisesRegex(RuntimeError, '报告与本次原始记录不一致'):
                run.verify_archive(out / '压测原始数据.tar.gz', report + 'tampered')

    def test_missing_dependency_replaces_previous_success_with_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            (out / '压测报告.md').write_text('old successful report')
            with patch.object(sys, 'argv', ['run.py', '--output', temp]), patch.object(run.shutil, 'which', return_value=None):
                self.assertEqual(run.main(), 1)
            report = (out / '压测报告.md').read_text()
            self.assertIn('缺少依赖', report)
            self.assertNotIn('old successful', report)
            self.assertNotIn('四项无损确认有效', report)


if __name__ == '__main__':
    unittest.main()
