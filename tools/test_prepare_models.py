"""确保模型损坏或文件缺失时不能通过校验。"""
import hashlib
import contextlib
import io
import json
from pathlib import Path
import tempfile
from unittest.mock import patch
import prepare_models
from prepare_models import valid

manifest = json.loads((prepare_models.ROOT / 'models.lock.json').read_text(encoding='utf-8'))
enabled = [m for m in manifest['models'] if m['release_enabled']]
assert (manifest['default_family'], manifest['default_tier']) == ('PP-OCRv6', 'small')
assert {(m['family'], m['tier']) for m in enabled} == {
    ('PP-OCRv6', 'tiny'), ('PP-OCRv6', 'small'), ('PP-OCRv6', 'medium'), ('PP-OCRv5', 'mobile')}
for family, tier in {(m['family'], m['tier']) for m in enabled}:
    assert {m['role'] for m in enabled if (m['family'], m['tier']) == (family, tier)} == {'det', 'rec'}

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'model'
    expected = {'size': 3, 'sha256': hashlib.sha256(b'abc').hexdigest()}
    assert not valid(path, expected)

    # 用本地 URL 走完整下载流程，检查进度及校验顺序，不访问网络或真实模型目录。
    source = Path(directory) / 'source.bin'
    data = b'x' * (1024 * 1024 + 1)
    source.write_bytes(data)
    entry = dict(name='inference.onnx', url=source.as_uri(), size=len(data),
                 sha256=hashlib.sha256(data).hexdigest())
    models = [dict(family='PP-OCRv6', tier='small', role=role,
                   repository='local-test', files=[entry]) for role in ('det', 'rec')]
    (Path(directory) / 'models.lock.json').write_text(json.dumps({'models': models}), encoding='utf-8')
    captured = io.StringIO()
    with patch.object(prepare_models, 'ROOT', Path(directory)), \
         patch('sys.argv', ['prepare_models.py']), contextlib.redirect_stdout(captured):
        prepare_models.main()
    output = captured.getvalue()
    assert output.count('100.0% (1,048,577 / 1,048,577 bytes)') == 2
    assert output.index('100.0%') < output.index('Verifying SHA256:') < output.index('OK ')
    for role in ('det', 'rec'):
        assert (Path(directory) / 'dist/models/PP-OCRv6/small' / role / 'inference.onnx').read_bytes() == data
    path.write_bytes(b'abc')
    assert valid(path, expected)
    path.write_bytes(b'abd')
    assert not valid(path, expected), 'Same-size corruption must be detected'
    path.write_bytes(b'ab')
    assert not valid(path, expected)
print('Model integrity checks passed')
