"""Isolated candidate-bridge proofs. Does not import or change either device SDK."""
import argparse
import atexit
import concurrent.futures
import gc
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import subprocess
import sys
import threading
import time


def load_native():
    import _transport_probe
    return _transport_probe


def setup():
    native = load_native()
    return native, native.create(f"proof/{os.getpid()}")


def submitted(native, context, device=0, fail=0):
    receipt = native.prepare(context)
    native.submit(receipt, device, fail)
    return receipt


class CandidateBridge:
    """Bounded completion metadata, with no second frame or TX scheduling queue."""
    def __init__(self, native, context):
        self.native = native
        self.context = context
        self.pending = {}
        self.sequence = 0
        self.publishing = set()

    def submit(self, device=0, fail=0, factory=concurrent.futures.Future):
        future = factory()
        future.native_receipt = self.native.prepare(self.context)
        token = self.sequence
        self.sequence += 1
        # Establish a strong, discoverable owner before native admission.
        self.pending[token] = future
        if not future.set_running_or_notify_cancel():
            del self.pending[token]
            raise concurrent.futures.CancelledError()
        try:
            self.native.submit(future.native_receipt, device, fail)
        except MemoryError:
            if not self.native.committed(future.native_receipt):
                del self.pending[token]
                raise
            # Admission happened: retain the original Future and its native receipt.
        except BaseException:
            if not self.native.committed(future.native_receipt):
                del self.pending[token]
            raise
        return future

    def harvest(self, result_factory=None):
        result_factory = result_factory or self.native.result
        for token, future in list(self.pending.items()):
            if token in self.publishing:
                continue
            if self.native.wait(future.native_receipt, 2000):
                result = result_factory(future.native_receipt)
                self.publishing.add(token)
                try:
                    future.set_result(result)
                finally:
                    self.publishing.remove(token)
                    if future.done():
                        self.native.retire(future.native_receipt)
                        self.pending.pop(token, None)


def receipt_ownership():
    native, context = setup()
    # A wrapper failure before admission must leave the backend untouched.
    def broken_wrapper():
        raise MemoryError("injected Python wrapper failure")
    try:
        broken_wrapper()
    except MemoryError:
        pass
    assert native.stats(context)[1:3] == (0, 0)
    bridge = CandidateBridge(native, context)
    try:
        bridge.submit(factory=broken_wrapper)
        raise AssertionError("wrapper allocation failure was hidden")
    except MemoryError:
        pass
    assert not bridge.pending and native.stats(context)[1:3] == (0, 0)
    try:
        native.prepare(context, 1)
        raise AssertionError("native capsule allocation fault did not fire")
    except MemoryError:
        pass
    assert native.stats(context)[1] == 0 and native.stats(context)[4] == 0
    future = bridge.submit(fail=1)
    receipt = future.native_receipt
    assert native.committed(receipt)
    assert not future.cancel()
    assert len(bridge.pending) == 1
    try:
        bridge.harvest(lambda _: broken_wrapper())
        raise AssertionError("result wrapping fault did not fire")
    except MemoryError:
        pass
    assert len(bridge.pending) == 1 and not future.done()
    future.add_done_callback(lambda _: bridge.harvest())
    bridge.harvest()
    assert not bridge.pending and future.result()[:2] == (1, 1)
    assert native.stats(context)[1:3] == (1, 1)
    try:
        native.submit(receipt, 0, 0)
        raise AssertionError("receipt reuse accepted")
    except ValueError:
        pass
    assert native.stats(context)[1] == 1


def retained_completed_futures():
    native, context = setup()
    bridge = CandidateBridge(native, context)
    history = []
    for _ in range(24):
        future = bridge.submit()
        history.append(future)
        bridge.harvest()
        assert future.result()[:2] == (1, 1)
        assert native.stats(context)[4] == 0
    native.close(context, 0)
    native.close(context, 1)
    assert all(native.result(future.native_receipt)[:2] == (1, 1) for future in history)


def bounded_receipts():
    native, context = setup()
    receipts = [native.prepare(context) for _ in range(8)]
    try:
        native.prepare(context)
        raise AssertionError("completion credits unbounded")
    except BufferError:
        pass
    assert native.stats(context)[1] == 0
    try:
        native.retire(receipts[0])
        raise AssertionError("unsubmitted receipt released its credit")
    except ValueError:
        pass
    receipts.clear()
    gc.collect()
    assert native.stats(context)[4] == 0
    receipt = submitted(native, context)
    assert native.wait(receipt, 2000)


def gil_wait_and_close():
    native, context = setup()
    native.gate(context, 0)
    receipt = submitted(native, context)
    assert native.gate(context, 1)
    assert not native.wait(receipt, 1)
    released = threading.Event()
    def release():
        time.sleep(0.05)
        native.gate(context, 2)
        released.set()
    thread = threading.Thread(target=release)
    thread.start()
    assert native.wait(receipt, 2000)
    thread.join(2)
    assert released.is_set() and not thread.is_alive()
    native.gate(context, 0)
    other = submitted(native, context)
    assert native.gate(context, 1)
    thread = threading.Thread(target=release)
    thread.start()
    native.close(context, 0)
    thread.join(2)
    assert not thread.is_alive()
    assert native.wait(other, 2000)
    peer = submitted(native, context, 1)
    assert native.wait(peer, 2000)


def callback_isolation():
    native, context = setup()
    a = submitted(native, context, 0)
    b = submitted(native, context, 1)
    first = concurrent.futures.Future()
    second = concurrent.futures.Future()
    entered = threading.Event()
    release = threading.Event()
    observed = []
    def blocked_callback(future):
        assert future.done()
        # Reentrant native diagnostics run outside writer and registry locks.
        native.stats(context)
        observed.append("first")
        entered.set()
        assert release.wait(3)
    def following_callback(_):
        observed.append("second")
    first.add_done_callback(blocked_callback)
    first.add_done_callback(following_callback)
    def finish(receipt, future):
        assert native.wait(receipt, 2000)
        future.set_result(native.result(receipt))
    thread = threading.Thread(target=finish, args=(a, first))
    thread.start()
    try:
        assert entered.wait(2)
        peer = threading.Thread(target=finish, args=(b, second))
        peer.start()
        assert second.result(2)[1] == 1
        peer.join(2)
        assert not peer.is_alive()
        assert observed == ["first"]
    finally:
        release.set()
        thread.join(3)
    assert observed == ["first", "second"]
    immediate = []
    first.add_done_callback(lambda _: immediate.append(True))
    assert immediate == [True]
    # A callback exception must not terminate native communication.
    def failing_callback(_):
        raise RuntimeError("injected callback exception")
    first.add_done_callback(failing_callback)
    last = submitted(native, context, 1)
    assert native.wait(last, 2000)


def shared_writer():
    native, context = setup()
    # Two independent consumer modules import one native extension. They are probes,
    # not the actual hand/arm packages, whose migration is deliberately out of scope.
    consumers = []
    for name in ("hand_probe", "arm_probe"):
        spec = importlib.util.spec_from_loader(name, loader=None)
        module = importlib.util.module_from_spec(spec)
        exec("import _transport_probe as native", module.__dict__)
        consumers.append(module)
    assert consumers[0].native is consumers[1].native is native
    try:
        native.create(f"proof/{os.getpid()}")
        raise AssertionError("a second Runtime bypassed the existing device registrations")
    except ValueError:
        pass
    native.gate(context, 0)
    receipts = [native.prepare(context) for _ in range(8)]
    errors = []
    def send(index):
        try:
            consumers[index % 2].native.submit(receipts[index], index % 2, 0)
        except BaseException as error:
            errors.append(repr(error))
    threads = [threading.Thread(target=send, args=(index,)) for index in range(8)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(2)
        assert not thread.is_alive()
    assert not errors
    assert native.gate(context, 1)
    assert native.stats(context)[0] == 1
    native.gate(context, 2)
    assert all(native.wait(receipt, 2000) for receipt in receipts)
    assert all(native.result(receipt)[:2] == (1, 1) for receipt in receipts)
    opens, accepted, frames, maximum, _, left, right = native.stats(context)
    assert (opens, accepted, frames, maximum) == (1, 8, 8, 1)
    assert left == right and left != 0


def destruction():
    native, context = setup()
    native.gate(context, 0)
    receipt = submitted(native, context)
    assert native.gate(context, 1)
    del context
    gc.collect()
    # The receipt pins its connection owner; it remains readable after the public
    # context wrapper is gone. Last-wrapper destruction releases the fake gate.
    assert native.committed(receipt)
    del receipt
    gc.collect()


def interpreter_exit():
    # Keep pending native work and wrappers alive until interpreter module teardown.
    native, context = setup()
    native.gate(context, 0)
    receipt = submitted(native, context)
    assert native.gate(context, 1)
    globals()["exit_context"] = context
    globals()["exit_receipt"] = receipt


def interpreter_exit_worker():
    native, context = setup()
    native.gate(context, 0)
    receipt = submitted(native, context)
    assert native.gate(context, 1)
    ready = threading.Event()
    finished = threading.Event()
    def finish():
        ready.set()
        assert native.wait(receipt, 3000)
        finished.set()
    worker = threading.Thread(target=finish, daemon=True)
    worker.start()
    assert ready.wait(2)
    deadline = time.monotonic() + 2
    while not native.waiting(receipt):
        assert time.monotonic() < deadline
        time.sleep(0.001)
    def shutdown():
        native.gate(context, 2)
        worker.join(3)
        if worker.is_alive() or not finished.is_set():
            os._exit(4)
    atexit.register(shutdown)


def interpreter_exit_callback():
    native, context = setup()
    receipt = submitted(native, context)
    future = concurrent.futures.Future()
    entered = threading.Event()
    release = threading.Event()
    def callback(_):
        entered.set()
        assert release.wait(5)
    future.add_done_callback(callback)
    def finish():
        assert native.wait(receipt, 2000)
        future.set_result(native.result(receipt))
    worker = threading.Thread(target=finish, daemon=True)
    worker.start()
    assert entered.wait(2)
    def shutdown():
        release.set()
        worker.join(3)
        if worker.is_alive():
            os._exit(5)
    atexit.register(shutdown)


SCENARIOS = {
    function.__name__: function for function in (
        receipt_ownership, retained_completed_futures, bounded_receipts, gil_wait_and_close,
        callback_isolation, shared_writer, destruction, interpreter_exit,
        interpreter_exit_worker, interpreter_exit_callback,
    )
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", choices=SCENARIOS)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()
    if args.scenario:
        SCENARIOS[args.scenario]()
        return
    report = {
        "scope": "candidate bridge with FakeTransport; no SDK migration or hardware claim",
        "python": sys.version, "platform": platform.platform(),
        "architecture": platform.machine(), "cases": [],
        "revision_from_ci": os.environ.get("GITHUB_SHA"),
        "native_archive_sha256": (
            hashlib.sha256(pathlib.Path(os.environ["PROOF_NATIVE_ARCHIVE"]).read_bytes()).hexdigest()
            if os.environ.get("PROOF_NATIVE_ARCHIVE") else None),
        "module_sha256": hashlib.sha256(pathlib.Path(load_native().__file__).read_bytes()).hexdigest(),
        "source_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in pathlib.Path(__file__).parent.iterdir()
                          if path.suffix in {".cpp", ".py", ".json"} or path.name == "CMakeLists.txt"},
    }
    for scenario in SCENARIOS:
        start = time.monotonic()
        try:
            result = subprocess.run(
                [sys.executable, "-X", "faulthandler", __file__, "--scenario", scenario],
                capture_output=True, text=True, timeout=15, check=False,
                env={**os.environ, "PYTHONMALLOC": "debug"},
            )
            case = {"name": scenario, "passed": result.returncode == 0,
                    "returncode": result.returncode, "stdout": result.stdout,
                    "stderr": result.stderr}
        except subprocess.TimeoutExpired as error:
            case = {"name": scenario, "passed": False, "timeout": str(error)}
        case["seconds"] = time.monotonic() - start
        report["cases"].append(case)
        print(scenario, "PASS" if case["passed"] else "FAIL", flush=True)
    report["passed"] = all(case["passed"] for case in report["cases"])
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not report["passed"]:
        print(json.dumps(report, indent=2))
        raise SystemExit(1)


if __name__ == "__main__":
    main()
