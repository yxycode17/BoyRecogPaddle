"""用真实截图验证当前 dist worker 的旋转输入坐标；不下载、不修改原图。"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import time

import cv2
import numpy as np
from test_native_worker import Client, ROOT


def inverse_rect(rect, angle, width, height):
    """半开矩形边界的逆变换，不将像素索引的 -1 混入边界坐标。"""
    left, top, right, bottom = rect
    if angle == 0:
        return list(rect)
    if angle == 90:
        return [top, height-right, bottom, height-left]
    if angle == 180:
        return [width-right, height-bottom, width-left, height-top]
    if angle == 270:
        return [width-bottom, left, width-top, right]
    raise ValueError(angle)


def iou(a, b):
    overlap = max(0, min(a[2], b[2])-max(a[0], b[0])) * max(0, min(a[3], b[3])-max(a[1], b[1]))
    area = (a[2]-a[0])*(a[3]-a[1])+(b[2]-b[0])*(b[3]-b[1])-overlap
    return overlap/area if area else 0


def compare(baseline, actual):
    # 按几何重合匹配，避免同一常见短词出现在不同位置时被误算为正确坐标。
    candidates = sorted(((iou(a['rect'], b['rect']), i, j)
                         for i, a in enumerate(baseline) for j, b in enumerate(actual)), reverse=True)
    used_a, used_b, matches = set(), set(), []
    for score, i, j in candidates:
        if score < .5:
            break
        if i in used_a or j in used_b:
            continue
        used_a.add(i)
        used_b.add(j)
        matches.append(dict(baseline=i, rotated=j, iou=score,
                            same_text=baseline[i]['text'] == actual[j]['text']))
    return dict(geometry_matches=len(matches), text_and_geometry_matches=sum(m['same_text'] for m in matches),
                median_iou=statistics.median(m['iou'] for m in matches) if matches else 0, matches=matches)


def check_mapping():
    # 非正方形、非对称框以及贴边框：使用真实像素旋转作为独立参照。
    for rect in ([0, 0, 13, 9], [2, 1, 8, 5], [0, 3, 4, 9], [12, 8, 13, 9]):
        mask = np.zeros((9, 13), np.uint8)
        mask[rect[1]:rect[3], rect[0]:rect[2]] = 1
        for angle in (0, 90, 180, 270):
            rotated = np.rot90(mask, -angle//90)
            ys, xs = np.nonzero(rotated)
            rotated_rect = [int(xs.min()), int(ys.min()), int(xs.max()+1), int(ys.max()+1)]
            assert inverse_rect(rotated_rect, angle, 13, 9) == list(rect)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    check_mapping()
    source = args.image.read_bytes()
    original = cv2.imdecode(np.frombuffer(source, np.uint8), cv2.IMREAD_COLOR)
    assert original is not None
    height, width = original.shape[:2]
    output = ROOT/'build/rotation-coordinates'
    output.mkdir(parents=True, exist_ok=True)
    exe = ROOT/'dist/worker/PaddleOcrWorker.exe'
    report = dict(image_sha256=hashlib.sha256(source).hexdigest(), width=width, height=height,
                  executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), model='PP-OCRv6 small',
                  angles_clockwise=True, inverse_mapping_selfcheck=True, modes={})
    for mode in ('gpu', 'cpu'):
        client = Client(exe, output/'config', ROOT/'dist/models/PP-OCRv6/small', mode)
        results = report['modes'][mode] = {}
        try:
            for angle in (0, 90, 180, 270):
                rotated = np.ascontiguousarray(np.rot90(original, -angle//90))
                ok, encoded = cv2.imencode('.png', rotated)
                assert ok
                start = time.perf_counter()
                response = client.request(1, encoded.tobytes())
                elapsed = (time.perf_counter()-start)*1000
                assert response['status'] == 0, response['error']
                assert (response['height'], response['width']) == rotated.shape[:2]
                mapped = [dict(rect=inverse_rect(item['rect'], angle, width, height), text=item['text'],
                               characters=[dict(text=c['text'],rect=inverse_rect(c['rect'],angle,width,height))
                                           for c in item['characters']])
                          for item in response['items']]
                assert all(0 <= x['rect'][0] < x['rect'][2] <= width and
                           0 <= x['rect'][1] < x['rect'][3] <= height for x in mapped)
                assert all(0 <= c['rect'][0] < c['rect'][2] <= width and
                           0 <= c['rect'][1] < c['rect'][3] <= height for x in mapped for c in x['characters'])
                baseline = mapped if angle == 0 else results['0']['mapped_items']
                results[str(angle)] = dict(response=response, mapped_items=mapped, elapsed_ms=elapsed,
                                          coordinates_in_bounds=True, comparison=compare(baseline, mapped))
                if mode == 'gpu':
                    overlay = original.copy()
                    for item in mapped:
                        left, top, right, bottom = item['rect']
                        cv2.rectangle(overlay, (left, top), (right-1, bottom-1), (0, 0, 255), 1)
                    cv2.imencode('.png', overlay)[1].tofile(str(output/f'mapped-{angle}.png'))
                print(mode, angle, 'items=', len(mapped), 'geometry=', results[str(angle)]['comparison']['geometry_matches'],
                      'same_text=', results[str(angle)]['comparison']['text_and_geometry_matches'], flush=True)
        finally:
            assert client.close() == 0
        (output/'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    report['gpu_cpu_exact'] = {angle: report['modes']['gpu'][angle]['response'] == report['modes']['cpu'][angle]['response']
                               for angle in ('0', '90', '180', '270')}
    (output/'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('GPU/CPU exact:', report['gpu_cpu_exact'], flush=True)


if __name__ == '__main__':
    main()
