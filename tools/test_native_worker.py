"""Windows 命名管道端到端检查；只在 build 中保存开发日志及结果。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import struct
import subprocess
import threading
import time
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
K = C.WinDLL('kernel32', use_last_error=True)
K.CreateNamedPipeW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, W.DWORD, W.DWORD, W.DWORD, W.DWORD, C.c_void_p]
K.CreateNamedPipeW.restype = W.HANDLE
K.ConnectNamedPipe.argtypes = [W.HANDLE, C.c_void_p]
K.ReadFile.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
K.WriteFile.argtypes = K.ReadFile.argtypes
K.CloseHandle.argtypes = [W.HANDLE]


class Client:
    def __init__(self, exe, config, model, mode='auto', family='PP-OCRv6', tier='small'):
        self.name = r'\\.\pipe\BoyRecogPaddle-' + uuid.uuid4().hex
        self.pipe = K.CreateNamedPipeW(self.name, 3 | 0x80000, 8, 1, 65536, 65536, 5000, None)
        if self.pipe == W.HANDLE(-1).value:
            raise C.WinError(C.get_last_error())
        self.process = subprocess.Popen([str(exe), '--pipe', self.name, '--parent-pid', str(os.getpid()),
            '--config-dir', str(config), '--model-dir', str(model), '--device', mode, '--family', family, '--tier', tier],
            creationflags=subprocess.CREATE_NO_WINDOW, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.timer = threading.Timer(140, self.process.kill)
        self.timer.start()
        if not K.ConnectNamedPipe(self.pipe, None) and C.get_last_error() != 535:
            self.close()
            raise C.WinError(C.get_last_error())

    def send(self, data):
        for offset in range(0, len(data), 32768):
            chunk = data[offset:offset+32768]
            done = W.DWORD()
            if not K.WriteFile(self.pipe, chunk, len(chunk), C.byref(done), None) or done.value != len(chunk):
                raise C.WinError(C.get_last_error())

    def read(self, size):
        result = bytearray()
        while len(result) < size:
            buffer = C.create_string_buffer(min(size-len(result), 65536))
            done = W.DWORD()
            if not K.ReadFile(self.pipe, buffer, len(buffer), C.byref(done), None) or not done.value:
                raise RuntimeError('Worker pipe disconnected')
            result.extend(buffer.raw[:done.value])
        return bytes(result)

    def request(self, operation, data=b'', request_id=7):
        self.send(struct.pack('<IIII', 0x31525042, operation, request_id, len(data))+data)
        magic, op, identity, size = struct.unpack('<IIII', self.read(16))
        assert (magic, op, identity) == (0x31525042, operation | 0x80000000, request_id)
        assert size <= 8*1024*1024
        body = self.read(size)
        status, width, height, count = struct.unpack_from('<IIII', body)
        offset, items = 16, []
        for _ in range(count):
            kind, left, top, right, bottom, size = struct.unpack_from('<IIIIII', body, offset)
            offset += 24
            text = body[offset:offset+size].decode('utf-8')
            offset += size
            assert kind == 3 and 0 <= left < right <= width and 0 <= top < bottom <= height
            items.append(dict(rect=[left, top, right, bottom], text=text))
        size, = struct.unpack_from('<I', body, offset)
        error = body[offset+4:offset+4+size].decode('utf-8')
        assert offset+4+size == len(body)
        return dict(status=status, width=width, height=height, items=items, error=error)

    def close(self):
        if self.pipe:
            K.CloseHandle(self.pipe)
            self.pipe = None
        try:
            code = self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('Worker failed to exit after disconnect')
        finally:
            self.timer.cancel()
        return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--exe', type=Path, default=ROOT/'build/Release/PaddleOcrWorker.exe')
    args = parser.parse_args()
    png = args.image.read_bytes()
    base = ROOT/'build/native-tests'
    base.mkdir(exist_ok=True)
    model = ROOT/'dist/models/PP-OCRv6/small'
    config = base/'配置'
    # ping 不加载模型；缺失模型返回失败，进程仍能处理控制请求。
    client = Client(args.exe, config, base/'missing')
    try:
        assert client.request(2)['status'] == 0
        error = client.request(1, png)
        assert error['status'] != 0 and not error['items'] and 'Missing model' in error['error']
        assert client.request(3)['status'] == 0
    finally:
        assert client.close() == 0
    assert 'Device ready' not in (config/'logs/worker.log').read_text()
    # 实际 CPU/GPU 结果、重复请求、坏图片及空白图。
    results = {}
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
    blank = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 64, 8, 2, 0, 0, 0))
    blank += chunk(b'IDAT', zlib.compress((b'\0'+b'\xff'*64*3)*64))+chunk(b'IEND', b'')
    for mode in ('gpu', 'cpu'):
        client = Client(args.exe, config, model, mode)
        try:
            assert client.request(2)['status'] == 0
            result = client.request(1, png)
            assert result['status'] == 0, result['error']
            assert result['items']
            assert client.request(1, png)['items'] == result['items']
            assert client.request(1, b'not png')['status'] != 0
            blank_result = client.request(1, blank)
            assert blank_result['status'] == 0 and not blank_result['items']
            results[mode] = result
        finally:
            assert client.close() == 0
    assert results['gpu'] == results['cpu']
    assert (config/'logs/worker.previous.log').exists()
    assert len(list((config/'logs').glob('*.log'))) == 2
    # 推理/模型加载进行中断开，而不是等待下一次读取才退出。
    client = Client(args.exe, config, model)
    client.send(struct.pack('<IIII', 0x31525042, 1, 9, len(png))+png)
    time.sleep(.05)
    start = time.monotonic()
    assert client.close() == 0
    disconnect_ms = (time.monotonic()-start)*1000
    # 已加载模型后在第二次真正推理期间断开。
    client = Client(args.exe, config, model, 'cpu')
    assert client.request(1, png)['status'] == 0
    client.send(struct.pack('<IIII', 0x31525042, 1, 11, len(png))+png)
    time.sleep(.05)
    start = time.monotonic()
    assert client.close() == 0
    inference_disconnect_ms = (time.monotonic()-start)*1000
    assert (config/'logs/worker.log').read_text().count('OCR completed') == 1
    corrupt = base/'corrupt'
    corrupt.mkdir(exist_ok=True)
    (corrupt/'profile.json').write_text('invalid profile', encoding='utf-8')
    client = Client(args.exe, config, corrupt)
    try:
        result = client.request(1, png)
        assert result['status'] != 0 and 'SHA256 mismatch' in result['error']
    finally:
        assert client.close() == 0
    # 畸形长度拒绝，不分配巨量负载。
    client = Client(args.exe, config, model)
    client.send(struct.pack('<IIII', 0x31525042, 1, 10, 0xffffffff))
    assert client.process.wait(timeout=3) != 0
    client.close()
    (base/'result.json').write_text(json.dumps(dict(results=results, disconnect_ms=disconnect_ms,
        inference_disconnect_ms=inference_disconnect_ms), ensure_ascii=False, indent=2), encoding='utf-8')
    print('Native pipe, OCR CPU/GPU, invalid input, missing model, log rotation and disconnect checks passed;',
          'items=', len(results['gpu']['items']), 'disconnect_ms=', round(disconnect_ms),
          'inference_disconnect_ms=', round(inference_disconnect_ms))


if __name__ == '__main__':
    main()
