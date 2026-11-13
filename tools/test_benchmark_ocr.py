"""最小后处理检查，不需要模型或网络。"""
import numpy as np
from benchmark_ocr import boxes_from_map, crop, decode

output = np.zeros((1, 6, 4), np.float32)
for t, i in enumerate([1, 1, 0, 1, 3, 2]):
    output[0, t, i] = 1
assert decode(output, ['', 'A', 'B', ' ']) == [('AA B', 1.0)]
try:
    decode(output, ['', 'A'])
    raise AssertionError('Dictionary mismatch was accepted')
except ValueError:
    pass
pred = np.zeros((64, 96), np.float32)
pred[20:35, 10:80] = .95
config = dict(thresh=.2, box_thresh=.45, max_candidates=1000, unclip_ratio=1.4)
boxes = boxes_from_map(pred, (128, 192, 3), config)
assert len(boxes) == 1
assert np.allclose(boxes[0].mean(axis=0), [89, 54], atol=2)
assert boxes[0].min() >= 0 and boxes[0][:, 0].max() < 192 and boxes[0][:, 1].max() < 128
assert crop(np.zeros((128, 192, 3), np.uint8), boxes[0]).size > 0
assert boxes_from_map(np.zeros_like(pred), (128, 192, 3), config) == []
print('CTC, DB, original-coordinate scaling and crop checks passed')
