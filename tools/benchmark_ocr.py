"""开发期真实图片 det/rec 对照；无网络，不是最终 C++ worker。"""
# DB 框后处理、裁切与 CTC 流程依据 PaddleOCR v3.7.0：
# ppocr/postprocess/db_postprocess.py、tools/infer/utility.py、predict_rec.py。
# Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
# See the License for the specific language governing permissions and limitations.
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import time

import cv2
import numpy as np
import onnxruntime as ort
import pyclipper
import yaml

from prepare_models import ROOT, valid


def quad(contour):
    rect = cv2.minAreaRect(np.asarray(contour, dtype=np.float32))
    points = sorted(cv2.boxPoints(rect), key=lambda p: p[0])
    left = sorted(points[:2], key=lambda p: p[1])
    right = sorted(points[2:], key=lambda p: p[1])
    return np.array([left[0], right[0], right[1], left[1]], np.float32), min(rect[1])


def boxes_from_map(pred, original_shape, config):
    contours, _ = cv2.findContours((pred > config['thresh']).astype(np.uint8),
                                   cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    height, width = pred.shape
    boxes = []
    for contour in contours[:config['max_candidates']]:
        box, short = quad(contour)
        if short < 3:
            continue
        lo = np.maximum(np.floor(box.min(axis=0)).astype(int), 0)
        hi = np.minimum(np.ceil(box.max(axis=0)).astype(int), [width-1, height-1])
        mask = np.zeros((hi[1]-lo[1]+1, hi[0]-lo[0]+1), np.uint8)
        cv2.fillPoly(mask, [(box-lo).astype(np.int32)], 1)
        score = cv2.mean(pred[lo[1]:hi[1]+1, lo[0]:hi[0]+1], mask)[0]
        if score < config['box_thresh']:
            continue
        perimeter = cv2.arcLength(box, True)
        if perimeter <= 0:
            continue
        offset = pyclipper.PyclipperOffset()
        offset.AddPath(box.astype(np.int64).tolist(), pyclipper.JT_ROUND, pyclipper.ET_CLOSEDPOLYGON)
        expanded = offset.Execute(cv2.contourArea(box)*config['unclip_ratio']/perimeter)
        if len(expanded) != 1:
            continue
        box, short = quad(expanded[0])
        if short < 5:
            continue
        box *= np.array([original_shape[1]/width, original_shape[0]/height])
        box = np.clip(np.rint(box), [0, 0], [original_shape[1]-1, original_shape[0]-1])
        if np.linalg.norm(box[0]-box[1]) > 3 and np.linalg.norm(box[0]-box[3]) > 3:
            boxes.append(box.astype(np.float32))
    return sorted(boxes, key=lambda b: (float(b[0, 1]), float(b[0, 0])))


def crop(image, box):
    width = int(max(np.linalg.norm(box[0]-box[1]), np.linalg.norm(box[2]-box[3])))
    height = int(max(np.linalg.norm(box[0]-box[3]), np.linalg.norm(box[1]-box[2])))
    dst = np.array([[0, 0], [width, 0], [width, height], [0, height]], np.float32)
    result = cv2.warpPerspective(image, cv2.getPerspectiveTransform(box, dst), (width, height),
                                 flags=cv2.INTER_CUBIC, borderMode=cv2.BORDER_REPLICATE)
    return np.rot90(result) if height / width >= 1.5 else result


def decode(output, dictionary):
    if output.ndim != 3 or output.shape[2] != len(dictionary) or not np.isfinite(output).all():
        raise ValueError('Invalid CTC output shape, dictionary or values')
    results = []
    for row in output:
        ids = row.argmax(axis=1)
        keep = (ids != 0) & np.r_[True, ids[1:] != ids[:-1]]
        results.append((''.join(dictionary[i] for i in ids[keep]),
                        float(row.max(axis=1)[keep].mean()) if keep.any() else 0.0))
    return results


class Pipeline:
    def __init__(self, root, mode, threads, profile=None):
        self.configs = {r: yaml.safe_load((root/r/'inference.yml').read_text(encoding='utf-8'))
                        for r in ('det', 'rec')}
        if self.configs['det']['PostProcess']['name'] != 'DBPostProcess' or self.configs['rec']['PostProcess']['name'] != 'CTCLabelDecode':
            raise ValueError('Unsupported model processing')
        self.dictionary = [''] + self.configs['rec']['PostProcess']['character_dict'] + [' ']
        self.sessions = {}
        for role in ('det', 'rec'):
            opts = ort.SessionOptions()
            opts.intra_op_num_threads = threads
            opts.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
            opts.enable_mem_pattern = mode != 'gpu'
            if profile:
                opts.enable_profiling = True
                opts.profile_file_prefix = str(profile) + '_' + role
            providers = ['CPUExecutionProvider']
            if mode == 'gpu':
                if 'DmlExecutionProvider' not in ort.get_available_providers():
                    raise RuntimeError('DirectML unavailable; no silent CPU benchmark fallback')
                providers.insert(0, ('DmlExecutionProvider', {'device_id': 0}))
            session = ort.InferenceSession(str(root/role/'inference.onnx'), opts, providers=providers)
            session.disable_fallback()
            if mode == 'gpu' and 'DmlExecutionProvider' not in session.get_providers():
                raise RuntimeError('DirectML session unavailable')
            if len(session.get_inputs()) != 1 or session.get_inputs()[0].type != 'tensor(float)':
                raise ValueError('Unsupported model input')
            self.sessions[role] = session

    def run(self, image, limit):
        start = time.perf_counter()
        h, w = image.shape[:2]
        ratio = min(1., limit/max(h, w))
        dh, dw = [max(32, int(round(x*ratio/32))*32) for x in (h, w)]
        det_ops = self.configs['det']['PreProcess']['transform_ops']
        norm = next(op['NormalizeImage'] for op in det_ops if 'NormalizeImage' in op)
        if norm['scale'] not in ('1./255.', '1/255', 1/255):
            raise ValueError('Unsupported normalization scale')
        array = cv2.resize(image, (dw, dh)).astype(np.float32)/255
        array = (array-np.array(norm['mean'], np.float32))/np.array(norm['std'], np.float32)
        session = self.sessions['det']
        output = session.run(None, {session.get_inputs()[0].name: array.transpose(2, 0, 1)[None]})[0]
        if output.shape != (1, 1, dh, dw) or not np.isfinite(output).all():
            raise ValueError('Invalid DB output')
        boxes = boxes_from_map(output[0, 0], image.shape, self.configs['det']['PostProcess'])
        det_ms = (time.perf_counter()-start)*1000
        crops = [crop(image, box) for box in boxes]
        items = [None]*len(boxes)
        rec_ops = self.configs['rec']['PreProcess']['transform_ops']
        channels, rh, baseline = next(op['RecResizeImg']['image_shape'] for op in rec_ops if 'RecResizeImg' in op)
        order = sorted(range(len(crops)), key=lambda i: crops[i].shape[1]/crops[i].shape[0])
        # ponytail: fixed batch 8 for reproducible comparisons; production resource tuning is separate.
        for offset in range(0, len(order), 8):
            batch = order[offset:offset+8]
            rw = max(baseline, int(rh*max(crops[i].shape[1]/crops[i].shape[0] for i in batch)))
            tensor = np.zeros((len(batch), channels, rh, rw), np.float32)
            for j, i in enumerate(batch):
                cw = min(rw, math.ceil(rh*crops[i].shape[1]/crops[i].shape[0]))
                resized = cv2.resize(crops[i], (cw, rh)).astype(np.float32)/255
                tensor[j, :, :, :cw] = ((resized-.5)/.5).transpose(2, 0, 1)
            session = self.sessions['rec']
            output = session.run(None, {session.get_inputs()[0].name: tensor})[0]
            for i, (text, score) in zip(batch, decode(output, self.dictionary), strict=True):
                items[i] = {'box': boxes[i].astype(int).tolist(), 'text': text, 'score': score}
        return items, {'total_ms': (time.perf_counter()-start)*1000, 'det_ms': det_ms}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('model', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--limit', type=int, default=2048)
    parser.add_argument('--threads', type=int, default=max(1, (os.cpu_count() or 2)//2))
    args = parser.parse_args()
    if args.runs < 1 or args.limit < 32 or args.threads < 1:
        parser.error('Invalid runs, limit or threads')
    manifest = json.loads((ROOT/'models.lock.json').read_text(encoding='utf-8'))
    selected = [m for m in manifest['models'] if m['family'] == args.model.parent.name and m['tier'] == args.model.name]
    if {m['role'] for m in selected} != {'det', 'rec'}:
        raise ValueError('Unknown model combination')
    for model in selected:
        for entry in model['files']:
            if not valid(args.model/model['role']/entry['name'], entry):
                raise ValueError('Missing or corrupt model; benchmark never downloads')
    raw = args.image.read_bytes()
    image = cv2.imdecode(np.frombuffer(raw, np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError('Cannot decode image')
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'image_sha256': hashlib.sha256(raw).hexdigest(), 'image_shape': image.shape,
              'model': str(args.model), 'limit': args.limit, 'batch': 8, 'threads': args.threads,
              'runtime': ort.__version__, 'modes': {}}
    cv2.setNumThreads(args.threads)
    for mode in ('gpu', 'cpu'):
        print(f'Start {mode}: {args.model}', flush=True)
        start = time.perf_counter()
        pipeline = Pipeline(args.model, mode, args.threads)
        load_ms = (time.perf_counter()-start)*1000
        first, first_time = pipeline.run(image, args.limit)
        print(mode, 'first_ms=', round(first_time['total_ms']), flush=True)
        samples = []
        for run_index in range(args.runs):
            items, timing = pipeline.run(image, args.limit)
            samples.append(timing)
            print(mode, f'run={run_index+1}/{args.runs}', 'ms=', round(timing['total_ms']), flush=True)
            if [(x['box'], x['text']) for x in items] != [(x['box'], x['text']) for x in first]:
                raise ValueError('Unstable boxes or text across repeated runs')
        report['modes'][mode] = {'load_ms': load_ms, 'first': first_time, 'runs': samples,
                                 'median_ms': statistics.median(x['total_ms'] for x in samples), 'items': first}
        print(mode, 'load_ms=', round(load_ms), 'first_ms=', round(first_time['total_ms']),
              'median_ms=', round(report['modes'][mode]['median_ms']), 'boxes=', len(first), flush=True)
        del pipeline
    gpu, cpu = [report['modes'][m]['items'] for m in ('gpu', 'cpu')]
    report['exact_boxes_and_text'] = [(x['box'], x['text']) for x in gpu] == [(x['box'], x['text']) for x in cpu]
    # 单独一次 profile，不把分析开销混入以上计时。
    pipeline = Pipeline(args.model, 'gpu', args.threads, args.output/'profile')
    pipeline.run(image, args.limit)
    report['provider_events'] = {}
    for role, session in pipeline.sessions.items():
        events = json.loads(Path(session.end_profiling()).read_text())
        counts = {}
        for event in events:
            provider = event.get('args', {}).get('provider')
            if provider:
                counts[provider] = counts.get(provider, 0)+1
        if not counts.get('DmlExecutionProvider'):
            raise RuntimeError('No DirectML execution evidence')
        report['provider_events'][role] = counts
    (args.output/'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    for mode in ('gpu', 'cpu'):
        (args.output/f'{mode}.txt').write_text('\n'.join(i['text'] for i in report['modes'][mode]['items']), encoding='utf-8')
    print('Saved', args.output/'result.json', 'exact=', report['exact_boxes_and_text'], flush=True)


if __name__ == '__main__':
    main()
