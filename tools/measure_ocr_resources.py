"""独立进程采样 OCR 内存；使用 Windows PSAPI/PDH，不用于延迟计时。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import threading
import time

import cv2
import numpy as np
from benchmark_ocr import Pipeline
from prepare_models import ROOT, valid


class MemoryCounters(C.Structure):
    _fields_ = [('cb', W.DWORD), ('PageFaultCount', W.DWORD)] + [
        (name, C.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize',
        'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]


class CounterValue(C.Structure):
    _fields_ = [('status', W.DWORD), ('value', C.c_longlong)]


class CounterItem(C.Structure):
    _fields_ = [('name', W.LPWSTR), ('value', CounterValue)]


class Sampler:
    def __init__(self, pid=None, process_handle=None):
        # 外部进程句柄由调用方持有，采样器不关闭它。
        if (pid is None) != (process_handle is None):
            raise ValueError('pid and process_handle must be specified together')
        self.pid = os.getpid() if pid is None else pid
        self.process_handle = -1 if process_handle is None else process_handle
        self.psapi = C.WinDLL('psapi', use_last_error=True)
        self.psapi.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(MemoryCounters), W.DWORD]
        self.psapi.GetProcessMemoryInfo.restype = W.BOOL
        self.pdh = C.WinDLL('pdh')
        self.pdh.PdhOpenQueryW.argtypes = [W.LPCWSTR, C.c_size_t, C.POINTER(W.HANDLE)]
        self.pdh.PdhAddEnglishCounterW.argtypes = [W.HANDLE, W.LPCWSTR, C.c_size_t, C.POINTER(W.HANDLE)]
        self.pdh.PdhCollectQueryData.argtypes = [W.HANDLE]
        self.pdh.PdhCloseQuery.argtypes = [W.HANDLE]
        self.pdh.PdhGetFormattedCounterArrayW.argtypes = [W.HANDLE, W.DWORD, C.POINTER(W.DWORD), C.POINTER(W.DWORD), C.c_void_p]
        self.query = W.HANDLE()
        self.check(self.pdh.PdhOpenQueryW(None, 0, C.byref(self.query)))
        self.counters = {}
        try:
            for key, name in [('gpu_dedicated', 'Dedicated Usage'), ('gpu_shared', 'Shared Usage')]:
                handle = W.HANDLE()
                self.check(self.pdh.PdhAddEnglishCounterW(self.query, f'\\GPU Process Memory(*)\\{name}', 0, C.byref(handle)))
                self.counters[key] = handle
        except BaseException:
            self.pdh.PdhCloseQuery(self.query)
            raise
        self.stop = threading.Event()
        self.peaks = {}
        self.samples = 0
        self.error = None

    @staticmethod
    def check(status):
        if status:
            raise RuntimeError(f'PDH error 0x{status & 0xffffffff:08x}')

    def gpu_memory(self, handle):
        size, count = W.DWORD(), W.DWORD()
        status = self.pdh.PdhGetFormattedCounterArrayW(handle, 0x400, C.byref(size), C.byref(count), None) & 0xffffffff
        if status == 0x800007d5:  # PDH_NO_DATA：当前无 GPU 进程实例。
            return 0
        if status != 0x800007d2:  # PDH_MORE_DATA
            self.check(status)
        if not size.value:
            return 0
        buffer = C.create_string_buffer(size.value)
        self.check(self.pdh.PdhGetFormattedCounterArrayW(handle, 0x400, C.byref(size), C.byref(count), buffer))
        items = C.cast(buffer, C.POINTER(CounterItem))
        values = []
        for i in range(count.value):
            item = items[i]
            if item.name.startswith(f'pid_{self.pid}_'):
                if item.value.status not in (0, 1):
                    self.check(item.value.status)
                values.append(item.value.value)
        return sum(values)

    def sample(self):
        memory = MemoryCounters()
        memory.cb = C.sizeof(memory)
        if not self.psapi.GetProcessMemoryInfo(W.HANDLE(self.process_handle), C.byref(memory), memory.cb):
            raise C.WinError(C.get_last_error())
        self.check(self.pdh.PdhCollectQueryData(self.query))
        values = {'working_set': memory.WorkingSetSize, 'private_commit': memory.PrivateUsage,
                  'os_peak_working_set': memory.PeakWorkingSetSize,
                  **{key: self.gpu_memory(handle) for key, handle in self.counters.items()}}
        for key, value in values.items():
            self.peaks[key] = max(self.peaks.get(key, 0), value)
        self.samples += 1
        return values

    def run(self):
        try:
            while not self.stop.wait(.05):
                self.sample()
        except BaseException as error:
            self.error = str(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('model', type=Path)
    parser.add_argument('mode', choices=('gpu', 'cpu'))
    parser.add_argument('output', type=Path)
    parser.add_argument('--threads', type=int, default=max(1, (os.cpu_count() or 2)//2))
    args = parser.parse_args()
    manifest = json.loads((ROOT/'models.lock.json').read_text(encoding='utf-8'))
    selected = [m for m in manifest['models'] if m['family'] == args.model.parent.name and m['tier'] == args.model.name]
    if {m['role'] for m in selected} != {'det', 'rec'}:
        raise ValueError('Unknown model combination')
    for model in selected:
        for entry in model['files']:
            if not valid(args.model/model['role']/entry['name'], entry):
                raise ValueError('Missing or corrupt model; no download')
    image = cv2.imdecode(np.frombuffer(args.image.read_bytes(), np.uint8), cv2.IMREAD_COLOR)
    if image is None or args.threads < 1:
        raise ValueError('Invalid image or threads')
    cv2.setNumThreads(args.threads)
    sampler = Sampler()
    thread = threading.Thread(target=sampler.run, daemon=True)
    try:
        baseline = sampler.sample()
        thread.start()
        pipeline = Pipeline(args.model, args.mode, args.threads)
        for index in range(2):
            items, _ = pipeline.run(image, 2048)
            print(args.model.name, args.mode, f'run={index+1}/2', f'boxes={len(items)}', flush=True)
        # 给系统 GPU 性能计数器一次刷新机会，模型仍驻留。
        time.sleep(1)
    finally:
        sampler.stop.set()
        if thread.is_alive():
            thread.join()
        try:
            sampler.sample()
        finally:
            sampler.pdh.PdhCloseQuery(sampler.query)
    if sampler.error:
        raise RuntimeError(sampler.error)
    if args.mode == 'gpu' and sampler.peaks['gpu_dedicated'] + sampler.peaks['gpu_shared'] == 0:
        raise RuntimeError('No GPU memory samples; cannot report zero as measured usage')
    result = dict(model=str(args.model), mode=args.mode, pid=os.getpid(), threads=args.threads,
                  limit=2048, batch=8, runs=2, sample_interval_ms=50, samples=sampler.samples,
                  baseline_bytes=baseline, peak_bytes=sampler.peaks)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({k: round(v/1048576, 1) for k, v in sampler.peaks.items()}), flush=True)


if __name__ == '__main__':
    main()
