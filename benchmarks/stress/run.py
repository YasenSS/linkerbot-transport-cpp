#!/usr/bin/env python3
"""Build local transport sources and produce an Ubuntu benchmark report + raw archive."""
import argparse
import csv
import datetime as dt
import fcntl
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import tarfile
import tempfile
import time
import threading

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RESULTS = HERE.parent / "results"
LARGE_BUFFER = 4 * 1024 * 1024
LABELS = {'memory-can': '内存回环经典 CAN', 'memory-fd': '内存回环 CAN FD',
          'vcan-can': 'vcan 经典 CAN', 'vcan-fd': 'vcan CAN FD'}


def now():
    return dt.datetime.now().astimezone().isoformat()


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def capture(command, required=True, timeout=30):
    p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    r = dict(command=command, exit_code=p.returncode, stdout=p.stdout, stderr=p.stderr)
    if required and p.returncode:
        raise RuntimeError(f'命令失败: {command}\n{p.stderr}')
    return r


def snapshot(source):
    git = lambda *args: capture(['git', '-C', str(source), *args])['stdout']
    # Include tracked content (also dirty/deleted files), nonignored new files and symlink targets.
    files = git('ls-files', '-z', '--cached', '--others', '--exclude-standard').split('\0')
    manifest = {}
    for name in sorted(set(filter(None, files))):
        p = source / name
        manifest[name] = digest(p) if p.is_file() else ('missing' if not p.exists() else 'directory')
    return dict(path=str(source), commit=git('rev-parse', 'HEAD').strip(),
                status=git('status', '--porcelain=v1', '--untracked-files=all'), files=manifest,
                fingerprint=hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest())


def same_source(a, b):
    return all(a[k] == b[k] for k in ('commit', 'status', 'fingerprint'))


def stop(process, privileged=False):
    if process.poll() is not None:
        return
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if privileged:
            capture(['sudo', '-n', 'kill', f'-{sig.name[3:]}', '--', f'-{process.pid}'], required=False)
        else:
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
        try:
            process.wait(timeout=5)
            return
        except subprocess.TimeoutExpired:
            continue
    raise RuntimeError('无法停止压测进程组')


def execute(command, stdout, stderr, timeout, privileged=False):
    """No orphan worker/peer processes on timeout or Python SIGINT/SIGTERM."""
    with Path(stdout).open('w') as out, Path(stderr).open('w') as err:
        p = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
        timed_out = False
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            stop(p, privileged)
        except BaseException:
            stop(p, privileged)
            raise
    return p.returncode, timed_out


def assess(record):
    """Treat worker exit, full accounting and both kernel socket counters as evidence."""
    r = record.get('result')
    errors = []
    if record['timed_out']:
        errors.append('超时')
    if record['exit_code'] != 0:
        errors.append(f"退出码 {record['exit_code']}")
    if not isinstance(r, dict) or r.get('pass') is not True:
        errors.append('压测校验未通过或 JSON 缺失')
    else:
        count = r.get('validated', r.get('received', 0))
        keys = ['tx_frames', 'rx_frames', 'submitted' if 'submitted' in r else 'sent']
        if count <= 0 or any(r.get(k) != count for k in keys):
            errors.append('完整收发计数不一致')
        if record['backend'] == 'vcan' and any(r.get(k) != count for k in ['peer_received', 'peer_sent']):
            errors.append('模拟端计数不一致')
        if any(r.get(k, -1) != 0 for k in ['overflows', 'route_misses', 'invalid_frames', 'inbox_pending']):
            errors.append('库健康计数异常')
        if not all(isinstance(r.get(k), (int, float)) and math.isfinite(r[k]) and r[k] >= 0 for k in
                   ['frames_per_second', 'seconds', 'roundtrip_p99_us_upper', 'cpu_percent_one_core', 'rss_final_kib']):
            errors.append('指标缺失')
        elif r['seconds'] < record['requested_seconds'] or r['seconds'] <= 0 or not math.isclose(r['frames_per_second'], count/r['seconds'], rel_tol=1e-8):
            errors.append('时长或吞吐计算不一致')
    if record['backend'] == 'vcan':
        probes = record.get('probes', [])
        for role in ['session', 'peer']:
            opened = [x for x in probes if x.get('role') == role and x.get('event') == 'open']
            closed = [x for x in probes if x.get('role') == role and x.get('event') == 'close']
            if len(opened) != 1 or len(closed) != 1:
                errors.append(f'{role} 内核计数缺失')
                continue
            if any(x['probe_errno'] or x['tune_errno'] for x in opened + closed):
                errors.append(f'{role} 内核计数/缓冲设置失败')
            if closed[0]['drops'] != opened[0]['drops']:
                errors.append(f"{role} 内核丢帧 {closed[0]['drops'] - opened[0]['drops']}")
            if record['buffer'] == 'large' and opened[0]['rcvbuf'] < LARGE_BUFFER:
                errors.append(f'{role} 接收缓冲不足')
    return errors


def drops(record, role):
    values = {p['event']: p['drops'] for p in record.get('probes', []) if p['role'] == role}
    return values['close'] - values['open'] if 'close' in values and 'open' in values else None


class Suite:
    def __init__(self, source, raw, quick):
        self.source, self.raw, self.quick = source, raw, quick
        self.build = raw / 'build'
        self.ns = f'lbt-stress-{os.getpid()}'
        self.created = False
        self.watch_stop = threading.Event()
        self.watcher = None
        self.source_changed = False
        self.records, self.confirmed = [], {}
        self.meta = dict(started_at=now(), quick=quick, source_before=snapshot(source), namespace=self.ns,
                         host=dict(uname=platform.uname()._asdict(), os=Path('/etc/os-release').read_text(),
                                   cpu=capture(['lscpu'])['stdout'], compiler=capture(['c++', '--version'])['stdout'],
                                   cpu_affinity=sorted(os.sched_getaffinity(0))),
                         buffer_requested_bytes=LARGE_BUFFER, errors=[])
        self.flush()

    def flush(self):
        save(self.raw / 'runs.json', self.records)
        save(self.raw / 'metadata.json', self.meta)
        save(self.raw / 'confirmed.json', self.confirmed)

    def ns_cmd(self, cmd):
        return ['sudo', '-n', 'ip', 'netns', 'exec', self.ns] + cmd

    def configure(self, cmd, required=True):
        r = capture(cmd, required=required)
        self.meta.setdefault('configuration', []).append(r)
        self.flush()
        return r

    def setup(self):
        print(f'本次网络命名空间：{self.ns}；临时目录：{self.raw}', flush=True)
        self.configure(['sudo', '-n', 'true'])
        self.meta['interfaces_before'] = capture(['ip', '-j', '-d', 'link', 'show'])
        self.configure(['sudo', '-n', 'modprobe', 'vcan'])
        self.configure(['sudo', '-n', 'ip', 'netns', 'add', self.ns])
        self.created = True
        self.configure(self.ns_cmd(['ip', 'link', 'add', 'vcan0', 'type', 'vcan']))
        self.configure(self.ns_cmd(['ip', 'link', 'set', 'vcan0', 'up']))
        command = ['cmake', '-S', str(HERE), '-B', str(self.build), '-DCMAKE_BUILD_TYPE=Release',
                   f'-DTRANSPORT_SOURCE={self.source}']
        self.meta['build_command'] = command
        for name, cmd in [('configure', command), ('compile', ['cmake', '--build', str(self.build), '-j', str(min(8, os.cpu_count() or 1))])]:
            code, timed_out = execute(cmd, self.raw / f'{name}.stdout', self.raw / f'{name}.stderr', 180)
            if code or timed_out:
                raise RuntimeError(f'{name} 构建失败或超时，请查原始记录')
        library, version, linked_source = (self.build / 'linked-library.txt').read_text().splitlines()
        library = Path(library)
        if linked_source != str(self.source) or not library.resolve().is_relative_to(self.build.resolve()):
            raise RuntimeError('链接来源不属于本次本地源码构建')
        self.meta['library'] = dict(path=str(library), sha256=digest(library), version=version, source=linked_source)
        for worker in ['throughput', 'profile']:
            link_map = (self.build / f'{worker}_worker.map').read_text()
            linked_paths = [line.split(None, 1)[1] for line in link_map.splitlines() if line.startswith('LOAD ')]
            if not any((self.build / path).resolve() == library.resolve() for path in linked_paths):
                raise RuntimeError(f'{worker} 链接映射缺少本次构建的库')
        commands = json.loads((self.build / 'compile_commands.json').read_text())
        if not any(str(self.source / 'src') in x['file'] for x in commands):
            raise RuntimeError('未实际编译所选本地源码')
        self.meta['source_after_build'] = snapshot(self.source)
        if not same_source(self.meta['source_before'], self.meta['source_after_build']):
            raise RuntimeError('构建期间源码变化，测试无效')
        code, timed_out = execute([str(self.build / 'throughput_worker'), '--contracts'],
                                  self.raw / 'contracts.json', self.raw / 'contracts.log', 60)
        self.meta['contracts'] = dict(exit_code=code, timed_out=timed_out,
                                     result=json.loads((self.raw / 'contracts.json').read_text()))
        if code or timed_out or not self.meta['contracts']['result'].get('pass'):
            raise RuntimeError('队列边界/恢复契约测试失败')
        self.flush()
        def watch_source():
            while not self.watch_stop.wait(1):
                try:
                    if not same_source(self.meta['source_before'], snapshot(self.source)):
                        self.source_changed = True
                        self.meta['errors'].append('测试期间检测到源码变化，测试无效（即使随后恢复）')
                        return
                except Exception as exc:
                    self.meta['errors'].append(f'源码监测异常：{exc}')
                    return
        self.watcher = threading.Thread(target=watch_source, daemon=True)
        self.watcher.start()

    def mode(self, fd):
        self.configure(self.ns_cmd(['ip', 'link', 'set', 'vcan0', 'down']))
        self.configure(self.ns_cmd(['ip', 'link', 'set', 'vcan0', 'mtu', '72' if fd else '16']))
        self.configure(self.ns_cmd(['ip', 'link', 'set', 'vcan0', 'up']))

    def run(self, backend, fd, phase, seconds, threads, batch, window, buffer='large', peer=None):
        seconds = min(seconds, 2) if self.quick else seconds
        name = f'{len(self.records):03d}-{phase}-{backend}-{"fd" if fd else "can"}-t{threads}-b{batch}-w{window}'
        worker = 'profile' if peer is not None else 'throughput'
        cmd = [str(self.build / f'{worker}_worker'), '--seconds', str(seconds), '--threads', str(threads),
               '--batch', str(batch), '--window', str(window), '--samples', str(self.raw / f'{name}.csv')]
        if fd:
            cmd.append('--fd')
        if peer is not None and peer:
            cmd += ['--raw-peer', '--peer-batch', str(peer)]
        if backend == 'vcan':
            if peer is None:
                cmd += ['--vcan', 'vcan0']
            env = [f'LD_PRELOAD={self.build / "libsocket_probe.so"}', f'LBT_PROBE_LOG={self.raw / (name + ".jsonl")}']
            if buffer == 'large':
                env += [f'LBT_SESSION_RCVBUF={LARGE_BUFFER}', f'LBT_PEER_RCVBUF={LARGE_BUFFER}']
            cmd = self.ns_cmd(['env', *env, *cmd])
        print(f'START {name} ({seconds}s)', flush=True)
        started = time.monotonic()
        before = capture(self.ns_cmd(['ip', '-j', '-s', 'link', 'show', 'vcan0'])) if backend == 'vcan' else None
        code, timed_out = execute(cmd, self.raw / f'{name}.json', self.raw / f'{name}.log', seconds + 20, backend == 'vcan')
        try:
            result = json.loads((self.raw / f'{name}.json').read_text())
        except (ValueError, OSError):
            result = None
        probe = self.raw / f'{name}.jsonl'
        try:
            probes = [json.loads(line) for line in probe.read_text().splitlines()] if probe.exists() else []
        except ValueError:
            probes = []
        record = dict(name=name, backend=backend, fd=fd, phase=phase, requested_seconds=seconds,
                      threads=threads, batch=batch, window=window, buffer=buffer, peer=peer,
                      command=cmd, exit_code=code, timed_out=timed_out, wall_seconds=time.monotonic()-started,
                      result=result, probes=probes, interface_before=before,
                      interface_after=capture(self.ns_cmd(['ip', '-j', '-s', 'link', 'show', 'vcan0'])) if backend == 'vcan' else None)
        samples = list(csv.DictReader(io.StringIO((self.raw / f'{name}.csv').read_text()))) if (self.raw / f'{name}.csv').exists() else []
        rss = [int(x['rss_kib']) for x in samples]
        record['rss_samples_kib'] = dict(min=min(rss), max=max(rss), first=rss[0], last=rss[-1]) if rss else None
        record['errors'] = assess(record)
        record['pass'] = not record['errors']
        if timed_out or result is None:
            self.meta['errors'].append(f'{name} 超时或缺失有效 JSON')
        self.records.append(record)
        self.flush()
        print(f"END {name}: " + (f"PASS {result['frames_per_second']:.0f} fps" if record['pass'] else '; '.join(record['errors'])), flush=True)
        return record

    def top(self, backend, fd, phases):
        return sorted([r for r in self.records if r['backend'] == backend and r['fd'] == fd and
                       r['phase'] in phases and r['pass']], key=lambda r: r['result']['frames_per_second'], reverse=True)

    def search(self, backend, fd):
        smoke = self.run(backend, fd, 'smoke', 2, 1, 1, 8)
        if not smoke['pass']:
            raise RuntimeError(f'{backend}/{fd} 基础收发失败')
        configs = [(t, b, 8) for b in [1, 8] for t in [1, 2, 4, 8, 16, 32]]
        configs += [(t, b, 1) for b in [32, 128] for t in [1, 8]]
        if self.quick:
            configs = [(1, 8, 8), (8, 128, 1)]
        for t, b, w in configs:
            self.run(backend, fd, 'screen', 15, t, b, w)
        best = self.top(backend, fd, ['screen'])
        if not best:
            raise RuntimeError('没有无损筛选候选')
        best = best[0]
        for w in ([1] if self.quick else [1, 4, 16, 32]):
            if best['threads'] * best['batch'] * w > 4096 or best['threads'] * w > 1024:
                continue
            if any(r['backend'] == backend and r['fd'] == fd and (r['threads'], r['batch'], r['window']) ==
                   (best['threads'], best['batch'], w) for r in self.records):
                continue
            self.run(backend, fd, 'window', 15, best['threads'], best['batch'], w)
        for r in self.top(backend, fd, ['screen', 'window'])[:2]:
            self.run(backend, fd, 'retest', 30, r['threads'], r['batch'], r['window'])
        for candidate in self.top(backend, fd, ['retest'])[:2]:
            trials = []
            for _ in range(2):
                r = self.run(backend, fd, 'confirm', 60, candidate['threads'], candidate['batch'], candidate['window'])
                if not r['pass']:
                    break
                trials.append(r['name'])
            if len(trials) == 2:
                self.confirmed[f'{backend}-{"fd" if fd else "can"}'] = trials
                self.flush()
                return
        raise RuntimeError(f'{backend}/{fd} 无配置通过两次连续确认')

    def diagnostics(self, fd):
        for _ in range(1 if self.quick else 3):
            self.run('vcan', fd, 'overload', 15, 8, 8, 8, 'default', peer=0)
        for phase, t, b, w, buffer, seconds in [('buffer-control', 8, 8, 8, 'large', 30),
                                               ('window-control', 8, 8, 1, 'default', 15)]:
            self.run('vcan', fd, phase, seconds, t, b, w, buffer, peer=0)
        for _ in range(1 if self.quick else 2):
            for t, w in [(2, 8), (8, 2), (8, 8)]:
                self.run('vcan', fd, 'matched', 15, t, 8, w, peer=0)
        if fd:
            # Identical raw peer code paths: change only read/write vs recvmmsg/sendmmsg.
            for peer in [0, 1, 32]:
                self.run('vcan', fd, 'peer-comparison', 60, 2, 8, 8, peer=peer)

    def cleanup(self):
        self.watch_stop.set()
        if self.watcher:
            self.watcher.join(timeout=40)
            if self.watcher.is_alive():
                self.meta["errors"].append("源码监测未正常结束")
        if self.created:
            pids = capture(['sudo', '-n', 'ip', 'netns', 'pids', self.ns], required=False)
            if pids['stdout'].strip():
                capture(['sudo', '-n', 'kill', '-KILL', *pids['stdout'].split()], required=False)
            result = capture(['sudo', '-n', 'ip', 'netns', 'delete', self.ns], required=False)
            self.meta['cleanup'] = result
            if result['exit_code']:
                self.meta['errors'].append('网络命名空间清理失败')
            self.created = False
        self.meta['source_after'] = snapshot(self.source)
        self.meta['source_unchanged'] = not self.source_changed and same_source(self.meta['source_before'], self.meta['source_after'])
        if not self.meta['source_unchanged']:
            self.meta['errors'].append('测试期间源码变化，测试无效')
        self.meta['finished_at'] = now()
        self.meta['interfaces_after'] = capture(['ip', '-j', '-d', 'link', 'show'], required=False)
        self.flush()


def render_report(meta, records, confirmed):
    library = meta.get('library', {})
    formal = not meta.get('quick')
    valid = len(confirmed) == 4 and meta.get('source_unchanged') is True and not meta.get('errors')
    by_name = {r['name']: r for r in records}
    failed = [r for r in records if not r['pass']]
    lines = [f"# Transport {library.get('version', '未知版本')} 压测报告" + ('' if formal else '（快速验证，非正式结果）'), '',
             '## 一、Ubuntu 系统压测', '', '### 测试信息', '',
             f"测试时间：{meta.get('started_at')} 至 {meta.get('finished_at', '未完成')}。",
             f"状态：{'正式流程完成，四项无损确认有效' if valid and formal else '快速验证完成（不可作为正式性能成绩）' if valid else '流程未完成或测试无效'}。共 {len(records)} 档，失败 {len(failed)} 档，失败档不计入无损成绩。", '',
             f"系统：{meta.get('host', {}).get('os', '未知').strip()}。",
             f"内核：{meta.get('host', {}).get('uname', {}).get('release', '未知')}；逻辑核：{len(meta.get('host', {}).get('cpu_affinity', []))}。", '',
             '```text', meta.get('host', {}).get('cpu', '未采集').strip(), '```', '',
             f"编译器：{meta.get('host', {}).get('compiler', '未采集').splitlines()[0]}。Release（CMake 默认 -O3 -DNDEBUG），C++17；库自身测试、示例、基准及依赖下载均关闭。", '',
             f"源码目录：`{meta.get('source_before', {}).get('path', '未知')}`；版本：`{library.get('version', '未知')}`。",
             f"提交：`{meta.get('source_before', {}).get('commit', '未知')}`。工作树：{'有本地变更，见压缩包 metadata.json' if meta.get('source_before', {}).get('status') else '干净'}。",
             f"源码 SHA256 指纹：`{meta.get('source_before', {}).get('fingerprint', '未采集')}`；测试期间源码{'保持一致' if meta.get('source_unchanged') else '未能确认一致，结果无效'}。",
             f"实际链接库：`{library.get('path', '未构建')}`，SHA256：`{library.get('sha256', '未构建')}`。静态库、链接映射、compile_commands.json、CMakeCache.txt 和构建日志均在原始数据包中。", '',
             '### 压测方式', '',
             '直接编译所选本地源码，不拉取远程、不使用发布包；每次在新的临时目录构建。内存回环与独立网络命名空间内 vcan 各测经典 CAN 8 字节、CAN FD+BRS 64 字节。',
             ('快速验证缩减扫描，仅用每档 2 秒覆盖四条路径、候选复测、两次确认及全部对照；以下均为非正式结果。' if not formal else
              '发送线程为 1/2/4/8/16/32，每批 1/8 帧、初始窗口 8 批；补测 1/8 线程、每批 32/128 帧、窗口 1 批，再调整最快候选窗口为 1/4/16/32 批（总在途不超过 4096 帧、1024 批）。每档筛选 15 秒，最快两个无损配置各复测 30 秒，择优连续两次确认 60 秒；若失败则尝试第二候选。'),
             '每线程提交窗口内各批，等发送凭据完成并按序收到所有回复后继续。校验 ID、帧标志、长度、序号及负载模式；以正确回复数量÷实际运行秒数计吞吐，一次请求与一次回复只计一次完整收发。经典 CAN 的 8 字节全部为序号及时间戳；FD 其余 56 字节也逐字节校验。',
             '正常 vcan 吞吐两端均申请 4 MiB 接收缓冲，只改变本次 socket。另测默认缓冲区 8 线程×8 帧×8 批过载，扩大缓冲和缩小窗口的对照；相同在途量比较线程数，并以相同 FD 配置比较逐帧、批量模拟端。',
             'P99 为 10µs 直方图桶上界（末桶使用实际最大值）。CPU 100% 表示一个逻辑核，包含压测程序、通信库和模拟端的进程 CPU，内核开销计入 system；RSS 为整个进程。软件回环成绩不代表物理 CAN 带宽。', '',
             '### 测试结果', '']
    for key, label in LABELS.items():
        trials = [by_name[n] for n in confirmed.get(key, [])]
        if len(trials) != 2 or not all(r['pass'] for r in trials):
            lines += [f'- **{label}：**未完成两次无损确认，不给出正式成绩。']
            continue
        a = trials[0]
        results = [r['result'] for r in trials]
        f = lambda metric, scale=1, digits=3: ' / '.join(f'{r[metric]/scale:.{digits}f}' for r in results)
        counts = ' / '.join(str(r.get('validated', r.get('received'))) for r in results)
        rss = [r['rss_samples_kib'] for r in trials if r['rss_samples_kib']]
        rss_text = f"{min(x['min'] for x in rss)/1024:.2f}～{max(x['max'] for x in rss)/1024:.2f}" if rss else '缺失'
        lines += [f"- **{label}：**两次确认 **{f('frames_per_second', 10000, 2)} 万次完整收发/秒**；"
                  f"{a['threads']} 线程，每批 {a['batch']} 帧，每线程窗口 {a['window']} 批，总在途上限 {a['threads']*a['batch']*a['window']} 帧。"
                  f"校验数量 **{counts}**；P99 **{f('roundtrip_p99_us_upper', 1000)} ms**；CPU **{f('cpu_percent_one_core', 100, 2)} 核**；"
                  f"RSS 采样范围 **{rss_text} MiB**；库内溢出 **0 / 0**；内核丢帧 **{'Session 0 / 0，模拟端 0 / 0' if a['backend']=='vcan' else '不适用（未经过内核 socket）'}**。"
                  f"原始记录：`{'`、`'.join(confirmed[key])}`。"]
    lines += ['', '实际 socket 接收缓冲（内核返回值，可能为申请值的两倍）：']
    for key in ['vcan-can', 'vcan-fd']:
        rs = [by_name[n] for n in confirmed.get(key, [])]
        if rs:
            sizes = ['/'.join(f"{p['role']}={p['rcvbuf']} B" for p in r['probes'] if p['event'] == 'open') for r in rs]
            lines.append(f"- {LABELS[key]}：" + '；'.join(sizes) + '。')
    lines += ['', '### 性能分析', '', '#### 1. 高并发下丢帧', '']
    for fd, label in [(False, '经典 CAN'), (True, 'CAN FD')]:
        overloads = [r for r in records if r['fd'] == fd and r['phase'] == 'overload']
        if not overloads:
            lines.append(f'{label}：未完成过载测试，证据不足。')
            continue
        for r in overloads + [x for x in records if x['fd'] == fd and x['phase'] in ['buffer-control', 'window-control']]:
            x = r['result'] or {}
            buffers = ', '.join(f"{p['role']}={p['rcvbuf']} B" for p in r['probes'] if p['event'] == 'open')
            lines += [f"- {label} `{r['name']}`：{'通过' if r['pass'] else '失败'}；{r['threads']}×{r['batch']}×{r['window']}，{r['buffer']} 缓冲（{buffers}）；"
                      f"提交/库发/模拟收/模拟回/库收/校验={x.get('submitted', '?')}/{x.get('tx_frames', '?')}/{x.get('peer_received', '?')}/{x.get('peer_sent', '?')}/{x.get('rx_frames', '?')}/{x.get('validated', '?')}；"
                      f"内核丢帧 Session={drops(r, 'session')}、模拟端={drops(r, 'peer')}；库内溢出={x.get('overflows', '?')}。"]
        lost_roles = {role for role in ['session', 'peer'] if any((drops(r, role) or 0) > 0 for r in overloads)}
        controls = [r for r in records if r['fd'] == fd and r['phase'] in ['buffer-control', 'window-control']]
        if lost_roles:
            roles = '、'.join('Session 接收端' if x == 'session' else '模拟回复接收端' for x in sorted(lost_roles))
            lines += [f"\n{label}：本轮内核计数确认 {roles} 的 socket 接收队列发生丢帧。"
                      + ('扩大缓冲及减少在途量两项对照均通过，支持突发接收队列过载的解释。' if len(controls) == 2 and all(r['pass'] for r in controls) else '部分对照未通过，不能仅据此排除其他原因。')
                      + ('这些过载记录未见库内溢出。' if all((r['result'] or {}).get('overflows') == 0 for r in overloads) else '库内溢出情况见各档计数。')]
        else:
            lines += [f'\n{label}：本轮未记录到默认缓冲的内核丢帧，未复现旧报告中的接收队列丢帧；失败原因需结合本次日志，不能沿用历史结论。']
        lines.append('')
    lines += ['#### 2. 性能瓶颈', '']
    for fd, label in [(False, '经典 CAN'), (True, 'CAN FD')]:
        groups = []
        for t, w in [(2, 8), (8, 2), (8, 8)]:
            rs = [r for r in records if r['fd'] == fd and r['phase'] == 'matched' and r['threads'] == t and r['window'] == w and r['pass']]
            if not rs:
                continue
            avg = lambda k: sum(r['result'][k] for r in rs) / len(rs)
            groups.append((t, w, avg('frames_per_second'), avg('roundtrip_p99_us_upper')))
            lines.append(f"- {label}：{t} 线程×8 帧×{w} 批，{t*8*w} 帧在途上限，{len(rs)} 次有效对照；"
                         f"平均 {avg('frames_per_second')/10000:.2f} 万次/秒，平均 P99 {avg('roundtrip_p99_us_upper')/1000:.3f} ms，"
                         f"CPU {avg('cpu_percent_one_core')/100:.2f} 核，模拟端 CPU {avg('peer_cpu_percent')/100:.2f} 核。")
        if len(groups) == 3:
            a, b, c = groups
            lines.append(f"\n{label}：相同 128 帧在途上限时，2→8 线程吞吐变化 {(b[2]/a[2]-1)*100:+.1f}%；"
                         f"8 线程将上限从 128→512 帧时，吞吐变化 {(c[2]/b[2]-1)*100:+.1f}%，平均 P99 从 {b[3]/1000:.3f}→{c[3]/1000:.3f} ms。差异受调度与运行波动影响，只支持本次配置间比较。\n")
        else:
            lines.append(f'{label} 对照有效数据不足，无法评价线程或在途量的影响。\n')
    peer_runs = [r for r in records if r['phase'] == 'peer-comparison' and r['pass']]
    for r in peer_runs:
        x = r['result']
        lines.append(f"- FD 相同 2×8×8、大缓冲配置，{'库模拟端逐帧' if r['peer']==0 else '原始 socket 模拟端逐帧' if r['peer']==1 else '原始 socket 模拟端批量 32 帧'}："
                     f"{x['frames_per_second']/10000:.2f} 万次/秒，P99 {x['roundtrip_p99_us_upper']/1000:.3f} ms，模拟端 CPU {x['peer_cpu_percent']/100:.2f} 核（`{r['name']}`）。")
    peers = {r['peer']: r for r in peer_runs}
    if 1 in peers and 32 in peers:
        gain = (peers[32]['result']['frames_per_second']/peers[1]['result']['frames_per_second']-1)*100
        lines += [f'\n仅将原始 socket 模拟端从逐帧改为批量后，本轮吞吐变化 {gain:+.1f}%；'
                  + ('说明测试端实现影响观测吞吐。' if abs(gain) >= 5 else '本轮未观察到超过 5% 的变化，无法据此认定模拟端是主导瓶颈。')]
    else:
        lines += ['\nFD 模拟端对照数据不足，不判断批量回复收益。']
    lines += ['本次没有单函数或锁的剖析证据，不能确定唯一瓶颈，也不能把有限参数搜索的最高成绩视为通信库的绝对上限。RSS 仅表示本次有限时长内的观测，不能证明长期无泄漏。', '', '### 失败配置与完整性', '']
    if failed:
        for r in failed:
            lines.append(f"- `{r['name']}`：" + '；'.join(r['errors']) + f"。原始 stdout/日志：`{r['name']}.json` / `{r['name']}.log`。")
    else:
        lines.append('本轮已执行档位均通过；仅代表上述实际覆盖范围。')
    for error in meta.get('errors', []):
        lines.append(f'- 流程异常：{error}')
    lines += ['', '原始数据见同目录 `压测原始数据.tar.gz`' + ('（快速验证使用 `快速验证原始数据.tar.gz`）' if not formal else '') + '。'
              '压缩包包含本报告副本、逐档 JSON/CSV/日志、socket 计数、运行环境、源码指纹、工具源码、编译参数和实际链接库。归档文件逐项 SHA256 校验后才清理临时目录。', '']
    return '\n'.join(lines)


def verify_archive(path, expected_report=None):
    with tarfile.open(path, 'r:gz') as archive:
        members = {m.name: m for m in archive.getmembers() if m.isfile()}
        read = lambda name: archive.extractfile(members[name]).read()
        manifest = json.loads(read('manifest.json'))
        if set(manifest) != set(members) - {'manifest.json'}:
            raise RuntimeError('压缩包文件清单不一致')
        for name, sha in manifest.items():
            if hashlib.sha256(read(name)).hexdigest() != sha:
                raise RuntimeError(f'归档校验失败: {name}')
        meta, runs, confirmed = [json.loads(read(name)) for name in ['metadata.json', 'runs.json', 'confirmed.json']]
        for r in runs:
            if r['result'] is not None and json.loads(read(r['name'] + '.json')) != r['result']:
                raise RuntimeError(f"原始 JSON 与汇总不一致: {r['name']}")
            if r['errors'] != assess(r) or r['pass'] != (not assess(r)):
                raise RuntimeError(f"有效性判定不一致: {r['name']}")
        if meta.get('library'):
            lib_member = 'build/' + str(Path(meta['library']['path']).relative_to(Path(meta['library']['path']).parents[1]))
            if hashlib.sha256(read(lib_member)).hexdigest() != meta['library']['sha256']:
                raise RuntimeError('实际链接库摘要与归档不一致')
        for r in runs:
            probe_name = r['name'] + '.jsonl'
            if probe_name in members and [json.loads(x) for x in read(probe_name).decode().splitlines()] != r['probes']:
                raise RuntimeError('socket 原始计数与汇总不一致')
            csv_name = r['name'] + '.csv'
            if csv_name in members:
                rss = [int(x['rss_kib']) for x in csv.DictReader(io.StringIO(read(csv_name).decode()))]
                expected = dict(min=min(rss), max=max(rss), first=rss[0], last=rss[-1]) if rss else None
                if expected != r['rss_samples_kib']:
                    raise RuntimeError('RSS 采样与汇总不一致')
        for key, names in confirmed.items():
            rs = [r for name in names for r in runs if r['name'] == name]
            if key not in LABELS or len(rs) != 2 or len(set(names)) != 2 or not all(r['pass'] and r['phase'] == 'confirm' and r['requested_seconds'] == (2 if meta['quick'] else 60) for r in rs):
                raise RuntimeError('确认数据不完整')
        report = render_report(meta, runs, confirmed)
        if read('report.md').decode() != report or (expected_report is not None and report != expected_report):
            raise RuntimeError('报告与本次原始记录不一致')
    return dict(records=len(runs), confirmed=len(confirmed), source_unchanged=meta.get('source_unchanged'), errors=meta['errors'])


def publish(raw, output, quick, meta, records, confirmed):
    report = render_report(meta, records, confirmed)
    (raw / 'report.md').write_text(report)
    # Include exactly the sources which ran, not __pycache__ or previous results.
    tools = raw / 'tool-source'
    tools.mkdir(exist_ok=True)
    for p in HERE.rglob("*"):
        if p.is_file() and p.suffix in ['.py', '.cpp', '.c', '.txt', '.md']:
            target = tools / p.relative_to(HERE)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, target)
    manifest = {str(p.relative_to(raw)): digest(p) for p in raw.rglob('*') if p.is_file() and p.name != 'manifest.json'}
    save(raw / 'manifest.json', manifest)
    prefix = '快速验证' if quick else '压测'
    archive_path, report_path = output / f'{prefix}原始数据.tar.gz', output / f'{prefix}报告.md'
    with tempfile.NamedTemporaryFile(prefix='.stress-archive-', dir=output, delete=False) as tmp:
        pending = Path(tmp.name)
    try:
        with tarfile.open(pending, 'w:gz') as archive:
            for p in sorted(raw.rglob('*')):
                if p.is_file():
                    archive.add(p, arcname=str(p.relative_to(raw)), recursive=False)
        verify_archive(pending, report)
        os.replace(pending, archive_path)
        with tempfile.NamedTemporaryFile(mode='w', prefix='.stress-report-', dir=output, delete=False) as tmp:
            tmp.write(report)
            pending = Path(tmp.name)
        os.replace(pending, report_path)
    finally:
        pending.unlink(missing_ok=True)
    print(f'OUTPUT {report_path}\nOUTPUT {archive_path}\n归档及报告一致性校验通过', flush=True)


def interrupted(signum, frame):
    raise KeyboardInterrupt(f'收到信号 {signum}')


def prepare_output(output, quick):
    if output is not None:
        output = output.resolve()
        output.mkdir(parents=True, exist_ok=True)
        return output
    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().astimezone().strftime('%Y%m%d-%H%M%S')
    base = RESULTS / f'{"quick" if quick else "stress"}-{stamp}'
    candidate = base
    counter = 1
    while True:
        try:
            candidate.mkdir()
            return candidate
        except FileExistsError:
            candidate = base.with_name(f'{base.name}-{counter:02d}')
            counter += 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT, help='本地源码目录，不自动同步远程')
    parser.add_argument('--quick', action='store_true', help='2 秒档位快速验证，单独命名，不产生正式成绩')
    parser.add_argument('--output', type=Path, help='输出目录；默认 benchmarks/results/{stress|quick}-时间戳/')
    parser.add_argument('--verify', type=Path, help='只校验已生成的原始数据包及报告指标')
    args = parser.parse_args()
    if args.verify:
        print(json.dumps(verify_archive(args.verify), ensure_ascii=False))
        return 0
    source = args.source.resolve()
    output = prepare_output(args.output, args.quick)
    print(f'结果目录：{output}', flush=True)
    # Serialize runs against the same source without a workspace lock file.
    lock_path = Path(tempfile.gettempdir()) / ('lbt-stress-' + hashlib.sha256(str(source).encode()).hexdigest()[:16] + '.lock')
    with lock_path.open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('同一源码已有压测在运行', flush=True)
            return 1
        signal.signal(signal.SIGINT, interrupted)
        signal.signal(signal.SIGTERM, interrupted)
        with tempfile.TemporaryDirectory(prefix='lbt-stress-') as temp:
            raw = Path(temp)
            suite = None
            meta = dict(started_at=now(), quick=args.quick, errors=[])
            try:
                for tool in ['cmake', 'c++', 'cc', 'git', 'ip', 'sudo', 'modprobe']:
                    if not shutil.which(tool):
                        raise RuntimeError(f'缺少依赖：{tool}')
                if platform.system() != 'Linux' or 'Ubuntu' not in Path('/etc/os-release').read_text():
                    raise RuntimeError('本工具仅覆盖 Ubuntu Linux')
                if not (source / 'CMakeLists.txt').is_file():
                    raise RuntimeError(f'无本地源码：{source}')
                suite = Suite(source, raw, args.quick)
                suite.setup()
                for fd in [False, True]:
                    suite.mode(fd)
                    for backend in ['memory', 'vcan']:
                        suite.search(backend, fd)
                    suite.diagnostics(fd)
            except BaseException as exc:
                meta = suite.meta if suite else meta
                meta['errors'].append(f'{type(exc).__name__}: {exc}')
                print(f'FAILED {exc}', flush=True)
            finally:
                if suite:
                    try:
                        suite.cleanup()
                    except BaseException as exc:
                        suite.meta['errors'].append(f'清理或源码复核失败：{exc}')
                    meta, records, confirmed = suite.meta, suite.records, suite.confirmed
                else:
                    records, confirmed = [], {}
                    meta['finished_at'] = now()
                save(raw / 'metadata.json', meta)
                save(raw / 'runs.json', records)
                save(raw / 'confirmed.json', confirmed)
                publish(raw, output, args.quick, meta, records, confirmed)
            valid = len(confirmed) == 4 and meta.get('source_unchanged') and not meta['errors']
            return 0 if valid else 1


if __name__ == '__main__':
    raise SystemExit(main())
