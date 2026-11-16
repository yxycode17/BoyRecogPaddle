"""重复同一批图片，记录原生 worker 请求结束后的内存；不用于延迟计时。

例如：python tools/test_worker_memory.py D:/share/png --mode gpu
GPU 计数器可能延迟更新；驻留、私有提交和 GPU 内存分别报告，不相加。
最后两轮逐页比较私有提交量；通过仅代表本次样本未超限，不能证明无泄漏。
"""
import argparse
import hashlib
import json
from pathlib import Path

from measure_ocr_resources import Sampler
from test_native_worker import Client, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('images', type=Path)
    parser.add_argument('--mode', choices=('gpu', 'cpu'), required=True)
    parser.add_argument('--exe', type=Path, default=ROOT/'dist/worker/PaddleOcrWorker.exe')
    parser.add_argument('--cycles', type=int, default=4)
    parser.add_argument('--max-growth-mib', type=float, default=64)
    parser.add_argument('--output', type=Path, default=ROOT/'build/worker-memory')
    args = parser.parse_args()
    files = sorted(args.images.glob('*.png'))
    if not files or args.cycles < 3 or not 0 <= args.max_growth_mib < float('inf'):
        parser.error('Need PNG images, at least 3 cycles and a finite nonnegative growth limit')
    output = (args.output/args.mode).resolve()
    output.mkdir(parents=True, exist_ok=True)
    client = Client(args.exe.resolve(), output, ROOT/'dist/models/PP-OCRv6/small', args.mode)
    # 总测试可超过 140 秒；worker 自身仍对每个请求实施 120 秒超时。
    client.timer.cancel()
    sampler = None
    rows, hashes, inputs = [], {}, {}
    try:
        sampler = Sampler(client.process.pid, int(client.process._handle))
        sampler.sample()
        for cycle in range(args.cycles):
            for index, path in enumerate(files):
                data = path.read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                assert inputs.setdefault(path.name, digest) == digest, 'Input changed during test'
                result = client.request(1, data, cycle*len(files)+index+1)
                assert result['status'] == 0, result['error']
                digest = hashlib.sha256(json.dumps(result, sort_keys=True).encode()).hexdigest()
                assert hashes.setdefault(path.name, digest) == digest, 'Repeated OCR output changed'
                # 确保上次请求的图片、结果、响应缓冲区已经销毁。
                assert client.request(2)['status'] == 0
                rows.append(dict(cycle=cycle+1, image=path.name, **sampler.sample()))
            print('cycle=', cycle+1, 'private_mib=', round(rows[-1]['private_commit']/1048576, 1),
                  'working_set_mib=', round(rows[-1]['working_set']/1048576, 1), flush=True)
    finally:
        if sampler:
            sampler.pdh.PdhCloseQuery(sampler.query)
        try:
            assert client.close() == 0
        finally:
            (output/'result.json').write_text(json.dumps(dict(mode=args.mode,
                exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                image_sha256=inputs, result_sha256=hashes, samples=rows), indent=2), encoding='utf-8')
    previous, last = rows[-2*len(files):-len(files)], rows[-len(files):]
    if args.mode == 'gpu':
        assert max(row['gpu_dedicated']+row['gpu_shared'] for row in rows) > 0, 'No GPU memory samples'
        assert 'Device ready: CPU' not in (output/'logs/worker.log').read_text(encoding='utf-8'), 'GPU fell back to CPU'
    growth = max(b['private_commit']-a['private_commit'] for a, b in zip(previous, last))
    print('Maximum same-page private commit growth MiB:', round(growth/1048576, 2))
    assert growth <= args.max_growth_mib*1048576, 'Worker private commit continues growing; inspect result.json'


if __name__ == '__main__':
    main()
