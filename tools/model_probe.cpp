// 开发期部署验证工具：真实执行两个模型，尚非完整 OCR worker。
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

static std::vector<float> Run(Ort::Session& session, const std::vector<int64_t>& shape) {
    const auto count = std::accumulate(shape.begin(), shape.end(), size_t{1},
        [](size_t a, int64_t b) { return a * static_cast<size_t>(b); });
    // 确定性输入用于检查执行与 CPU/GPU 一致性，不用于宣称 OCR 准确率。
    std::vector<float> input(count);
    for (size_t i = 0; i < count; ++i) input[i] = static_cast<float>(i % 251) / 125.0f - 1.0f;
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
    Ort::AllocatorWithDefaultOptions allocator;
    auto in = session.GetInputNameAllocated(0, allocator);
    auto out = session.GetOutputNameAllocated(0, allocator);
    const char* ins[] = {in.get()};
    const char* outs[] = {out.get()};
    auto result = session.Run(Ort::RunOptions{nullptr}, ins, &tensor, 1, outs, 1);
    const auto info = result[0].GetTensorTypeAndShapeInfo();
    const auto n = info.GetElementCount();
    if (n == 0 || info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("Invalid model output");
    const auto* values = result[0].GetTensorData<float>();
    if (!std::all_of(values, values + n, [](float v) { return std::isfinite(v); }))
        throw std::runtime_error("Non-finite model output");
    std::cout << " output_shape=";
    for (auto dim : info.GetShape()) std::cout << dim << ',';
    return {values, values + n};
}

static std::vector<float> Probe(Ort::Env& env, const std::filesystem::path& model,
                              const std::vector<int64_t>& shape, int device) {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2)));
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    if (device >= 0) {
        options.DisableMemPattern();
        const OrtDmlApi* api = nullptr;
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
            reinterpret_cast<const void**>(&api)));
        Ort::ThrowOnError(api->SessionOptionsAppendExecutionProvider_DML(options, device));
    }
    // 输出 EP 分配证据供检查，不将会话创建成功当作全图在 GPU 执行。
    const std::wstring profile = L"probe_" + model.parent_path().filename().wstring()
        + L"_" + std::to_wstring(device);
    options.EnableProfiling(profile.c_str());
    const auto start = std::chrono::steady_clock::now();
    Ort::Session session(env, model.c_str(), options);
    if (session.GetInputCount() != 1 || session.GetOutputCount() != 1)
        throw std::runtime_error("Expected single input and output");
    std::cout << model.parent_path().filename().string() << " device=" << device;
    auto result = Run(session, shape);
    auto second = Run(session, shape);
    if (result.size() != second.size()) throw std::runtime_error("Unstable output shape");
    Ort::AllocatorWithDefaultOptions allocator;
    auto path = session.EndProfilingAllocated(allocator);
    std::cout << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count() << " profile=" << path.get() << '\n';
    return result;
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3 || (std::wstring(argv[2]) != L"gpu" && std::wstring(argv[2]) != L"cpu")) {
            std::cerr << "Usage: model_probe <tier-directory> <gpu|cpu>\n";
            return 2;
        }
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "model_probe");
        int device = -1;
        if (std::wstring(argv[2]) == L"gpu") {
            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("DXGI unavailable");
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(adapter->GetDesc1(&desc)) || desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                device = static_cast<int>(index);
                std::wcout << L"GPU adapter=" << index << L" name=" << desc.Description << L'\n';
                break;
            }
            if (device < 0) throw std::runtime_error("No hardware adapter; GPU test must not silently use CPU");
        }
        const std::filesystem::path root(argv[1]);
        for (const auto& role : {L"det", L"rec"}) {
            const auto model = root / role / L"inference.onnx";
            const std::vector<int64_t> shape = std::wstring(role) == L"det"
                ? std::vector<int64_t>{1, 3, 640, 640} : std::vector<int64_t>{1, 3, 48, 320};
            auto result = Probe(env, model, shape, device);
            if (device >= 0) {
                auto cpu = Probe(env, model, shape, -1);
                if (result.size() != cpu.size()) throw std::runtime_error("CPU/GPU shape mismatch");
                float max_error = 0;
                for (size_t i = 0; i < cpu.size(); ++i) max_error = std::max(max_error, std::abs(cpu[i] - result[i]));
                std::cout << "max_abs_cpu_gpu=" << max_error << '\n';
                if (max_error > 0.01f) throw std::runtime_error("CPU/GPU numerical mismatch exceeds 0.01");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
