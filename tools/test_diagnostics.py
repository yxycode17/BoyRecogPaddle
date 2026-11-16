"""验证诊断开关和分段日志；不记录图片/文字到日志，不写文档目录。"""
import argparse
import json
import re
from pathlib import Path
from test_native_worker import Client, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--model', choices=('v6-small', 'v5-mobile'), default='v6-small')
    args = parser.parse_args()
    png = args.image.read_bytes()
    family, tier = ('PP-OCRv6', 'small') if args.model == 'v6-small' else ('PP-OCRv5', 'mobile')
    model_root = ROOT/('dist/models' if tier == 'small' else 'downloads/models')/family/tier
    output = ROOT/'build/diagnostics-test'/args.model
    report = {}
    for mode in ('gpu', 'cpu'):
        config = output/mode
        config.mkdir(parents=True, exist_ok=True)
        ini = config/'settings.ini'
        ini.write_text('[PaddleOCR]\ndiagnostics=1\n', encoding='utf-8')
        client = Client(ROOT/'dist/worker/PaddleOcrWorker.exe', config, model_root, mode, family, tier)
        log_path = config/'logs/worker.log'
        try:
            baseline = client.request(1, png, 1)
            assert baseline['status'] == 0
            # ping 确保上一请求日志写入已完成。
            assert client.request(2)['status'] == 0
            cold = log_path.read_text(encoding='utf-8')
            for stage in ('model_verify', 'session_det', 'session_rec', 'warmup_det', 'warmup_rec',
                          'png_decode', 'det_preprocess', 'det_inference', 'det_postprocess',
                          'crop_and_sort', 'rec_preprocess', 'rec_inference', 'ctc_decode',
                          'ocr_total', 'response_encode', 'pipe_receive', 'pipe_send', 'request_memory'):
                assert 'stage='+stage in cold, stage
            memory = re.search(r'stage=request_memory working_set_bytes=(\d+) private_commit_bytes=(\d+)', cold)
            assert memory and all(int(value) > 0 for value in memory.groups())
            assert 'shape_changed=' in cold and 'run_ms=' in cold and 'request_id=1' in cold
            if mode == 'gpu':
                assert 'name=' in cold and 'shared_bytes=' in cold
            ini.write_text('[PaddleOCR]\ndiagnostics=0\n', encoding='utf-8')
            assert client.request(1, png, 2) == baseline
            assert client.request(2)['status'] == 0
            disabled = log_path.read_text(encoding='utf-8')
            assert disabled.count('DIAG ') == cold.count('DIAG ')
            ini.write_text('[PaddleOCR]\ndiagnostics=1\n', encoding='utf-8')
            assert client.request(1, png, 3) == baseline
            assert client.request(2)['status'] == 0
            resumed = log_path.read_text(encoding='utf-8')
            assert 'DIAG request_id=3 stage=model_ready loaded_now=0' in resumed
            assert resumed.count('stage=request_memory ') == 2
            assert resumed.count('stage=model_verify begin') == 1
            for item in baseline['items']:
                if len(item['text']) >= 10 and any('\u4e00' <= c <= '\u9fff' for c in item['text']):
                    assert item['text'] not in resumed
            report[mode] = dict(items=len(baseline['items']), pid=client.process.pid,
                                live_toggle=True, identical_output=True, model_loaded_once=True)
        finally:
            assert client.close() == 0
    (output/'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Diagnostic stages, GPU/CPU, live toggle, model reuse and text privacy checks passed')


if __name__ == '__main__':
    main()
