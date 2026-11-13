"""检查 Windows 内存计数器的字节单位和分配响应。"""
import ctypes as C
from measure_ocr_resources import Sampler, CounterValue, CounterItem

assert C.sizeof(CounterValue) == 16 and C.sizeof(CounterItem) == 24
sampler = Sampler()
try:
    before = sampler.sample()
    allocation = bytearray(32*1024*1024)
    for offset in range(0, len(allocation), 4096):
        allocation[offset] = 1
    after = sampler.sample()
    assert after['private_commit'] - before['private_commit'] >= 16*1024*1024
    assert after['working_set'] > 0
    assert after['os_peak_working_set'] >= after['working_set']
    assert 0 <= after['gpu_dedicated'] < 1024**4
    assert 0 <= after['gpu_shared'] < 1024**4
finally:
    sampler.pdh.PdhCloseQuery(sampler.query)
print('Windows memory counter layout, byte units and allocation checks passed')
