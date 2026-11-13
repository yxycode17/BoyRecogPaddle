"""复测四套发布模型的原生 worker 管道端到端耗时；无下载。"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import time
from test_native_worker import Client, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--exe', type=Path, default=ROOT/'build/Release/PaddleOcrWorker.exe')
    parser.add_argument('--output', type=Path, default=ROOT/'build/native-benchmark')
    args = parser.parse_args()
    data = args.image.read_bytes()
    output = args.output.resolve()
    output.mkdir(exist_ok=True)
    report = dict(image_sha256=hashlib.sha256(data).hexdigest(),
                  executable_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(), models={})
    models = [('PP-OCRv6', 'tiny'), ('PP-OCRv6', 'small'), ('PP-OCRv6', 'medium'), ('PP-OCRv5', 'mobile')]
    for family, tier in models:
        root = ROOT/('dist/models' if tier == 'small' else 'downloads/models')/family/tier
        key = family+'-'+tier
        report['models'][key] = {}
        for mode in ('gpu', 'cpu'):
            start = time.perf_counter()
            client = Client(args.exe, output/'config', root, mode, family, tier)
            try:
                assert client.request(2)['status'] == 0
                startup_ms = (time.perf_counter()-start)*1000
                samples, first = [], None
                for index in range(4):
                    start = time.perf_counter()
                    result = client.request(1, data, index+1)
                    elapsed = (time.perf_counter()-start)*1000
                    assert result['status'] == 0, result['error']
                    if first is None:
                        first = result
                    else:
                        assert result == first, 'Unstable repeated OCR output'
                    samples.append(elapsed)
                    print(key, mode, f'run={index+1}/4', 'ms=', round(elapsed), 'items=', len(result['items']), flush=True)
                report['models'][key][mode] = dict(startup_ms=startup_ms, first_ms=samples[0],
                    warm_ms=samples[1:], median_ms=statistics.median(samples[1:]), result=first)
            finally:
                assert client.close() == 0
        values = report['models'][key]
        values['exact_cpu_gpu'] = values['gpu']['result'] == values['cpu']['result']
        reference = json.loads((ROOT/f'build/benchmark-{family[6:].lower()}-{tier}/result.json').read_text(encoding='utf-8'))
        expected = []
        for item in reference['modes']['gpu']['items']:
            xs, ys = zip(*item['box'])
            expected.append(dict(rect=[min(xs), min(ys), min(max(xs)+1, reference['image_shape'][1]),
                                       min(max(ys)+1, reference['image_shape'][0])], text=item['text']))
        actual = values['gpu']['result']['items']
        values['python_reference_exact'] = actual == expected
        values['reference_counts'] = [len(expected), len(actual)]
        # 框相同的项目逐项对照，不能把阅读顺序变化误算为所有文字不同。
        ref_by_rect = {tuple(x['rect']): x['text'] for x in expected}
        values['same_rect_text_matches'] = sum(ref_by_rect.get(tuple(x['rect'])) == x['text'] for x in actual)
        (output/'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(key, 'CPU/GPU exact=', values['exact_cpu_gpu'], 'reference exact=', values['python_reference_exact'], flush=True)
    assert all(m['exact_cpu_gpu'] for m in report['models'].values())
    print('Saved', output/'result.json', flush=True)


if __name__ == '__main__':
    main()
