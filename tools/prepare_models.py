"""开发期按固定清单准备模型；普通识别运行时不调用此脚本。"""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def valid(path, entry):
    if not path.is_file() or path.stat().st_size != entry['size']:
        return False
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest() == entry['sha256']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--family', choices=('PP-OCRv5', 'PP-OCRv6'), default='PP-OCRv6')
    parser.add_argument('--tier', choices=('tiny', 'small', 'medium', 'mobile'), default='small')
    parser.add_argument('--verify', action='store_true', help='只校验本地文件，不联网')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/models', help='模型根目录')
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'models.lock.json').read_text(encoding='utf-8'))
    selected = [m for m in manifest['models'] if m['family'] == args.family and m['tier'] == args.tier
                and m.get('release_enabled', True)]
    if {m['role'] for m in selected} != {'det', 'rec'}:
        parser.error('Unsupported family/tier combination')
    for model in selected:
        folder = args.output / args.family / args.tier / model['role']
        for entry in model['files']:
            path = folder / entry['name']
            if not valid(path, entry):
                if args.verify:
                    raise RuntimeError(f'Missing or invalid file: {path}')
                folder.mkdir(parents=True, exist_ok=True)
                print(f'Download {model["repository"]}/{entry["name"]}', flush=True)
                print(f'  0.0% (0 / {entry["size"]:,} bytes)', flush=True)
                # 开发期逐文件原子替换；运行时整档位发布另由设置下载流程实现。
                with tempfile.NamedTemporaryFile(dir=folder, delete=False) as output:
                    temp = Path(output.name)
                    try:
                        with urllib.request.urlopen(entry['url'], timeout=60) as response:
                            received = 0
                            last_progress = time.monotonic()
                            while chunk := response.read(1024 * 1024):
                                received += len(chunk)
                                if received > entry['size']:
                                    raise RuntimeError('Download exceeds expected size')
                                output.write(chunk)
                                now = time.monotonic()
                                if now - last_progress >= 0.5 or received == entry['size']:
                                    print(f'  {received / entry["size"]:.1%} '
                                          f'({received:,} / {entry["size"]:,} bytes)', flush=True)
                                    last_progress = now
                    except BaseException:
                        output.close()
                        temp.unlink(missing_ok=True)
                        raise
                try:
                    print(f'Verifying SHA256: {entry["name"]}', flush=True)
                    if not valid(temp, entry):
                        raise RuntimeError(f'Checksum mismatch: {path}')
                    temp.replace(path)
                finally:
                    temp.unlink(missing_ok=True)
            print(f'OK {path}', flush=True)


if __name__ == '__main__':
    main()
