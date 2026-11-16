// DB/CTC 流程依据 PaddleOCR v3.7.0，Copyright (c) 2020 PaddlePaddle Authors.
// Licensed under Apache-2.0: http://www.apache.org/licenses/LICENSE-2.0
// Distributed AS IS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
#include "ocr.hpp"
#include "model_hashes.hpp"
#include "model_verify.hpp"
#include "diagnostics.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/geometry.hpp>
#include <opencv2/imgcodecs.hpp>
#include <clipper.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

static constexpr int GpuBatchLimit(int availableBatch, SIZE_T dedicatedBytes, bool unifiedMemory) {
    return !unifiedMemory && dedicatedBytes < 4ULL*1024*1024*1024
        ? std::min(availableBatch, 2) : availableBatch;
}
static_assert(GpuBatchLimit(8, 128*1024*1024, true) == 8);
static_assert(GpuBatchLimit(1, 128*1024*1024, true) == 1);
static_assert(GpuBatchLimit(8, 2ULL*1024*1024*1024, false) == 2);
static_assert(GpuBatchLimit(8, 8ULL*1024*1024*1024, false) == 8);
static_assert(GpuBatchLimit(8, 0, false) == 2);

static std::string ReadText(const fs::path& path) {
    if (fs::file_size(path) > 4 * 1024 * 1024) throw std::runtime_error("Profile too large");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open profile");
    return {std::istreambuf_iterator<char>(input), {}};
}

static std::vector<cv::Point2f> Quad(const std::vector<cv::Point2f>& contour, float& shortSide) {
    const auto rect = cv::minAreaRect(contour);
    cv::Point2f p[4]; rect.points(p);
    std::sort(p, p + 4, [](auto a, auto b) { return a.x < b.x; });
    if (p[0].y > p[1].y) std::swap(p[0], p[1]);
    if (p[2].y > p[3].y) std::swap(p[2], p[3]);
    shortSide = std::min(rect.size.width, rect.size.height);
    return {p[0], p[2], p[3], p[1]};
}

struct Ocr::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "PaddleOcrWorker"};
    std::unique_ptr<Ort::Session> det, rec;
    fs::path root;
    LogFunction log;
    bool diagnostics=false;
    std::string deviceInfo="CPU";
    std::vector<int64_t> lastDetShape,lastRecShape;
    int device = -1, threads = 1, batch = 1, recHeight = 48, recWidth = 320, candidates = 1000;
    float threshold = 0, boxThreshold = 0, unclip = 0;
    std::vector<float> mean, stddev;
    std::vector<std::string> dictionary;

    Ort::Value Run(Ort::Session& session, std::vector<float>& input, const std::vector<int64_t>& shape,const char* stage) {
        auto started=DiagnosticClock::now();
        auto& previous=&session==det.get()?lastDetShape:lastRecShape;
        bool changed=previous!=shape;previous=shape;
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        auto in = session.GetInputNameAllocated(0, allocator), out = session.GetOutputNameAllocated(0, allocator);
        const char* ins[] = {in.get()}, *outs[] = {out.get()};
        auto runStart=DiagnosticClock::now();
        auto result = session.Run(Ort::RunOptions{nullptr}, ins, &tensor, 1, outs, 1);
        auto runEnd=DiagnosticClock::now();
        if (!result[0].IsTensor() || result[0].GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("Invalid model output type");
        const auto n = result[0].GetTensorTypeAndShapeInfo().GetElementCount();
        const auto* data = result[0].GetTensorData<float>();
        if (!std::all_of(data, data + n, [](float x) { return std::isfinite(x); }))
            throw std::runtime_error("Non-finite model output");
        if(diagnostics){
            std::string dimensions;for(auto dim:shape){if(!dimensions.empty())dimensions+='x';dimensions+=std::to_string(dim);}
            log("DIAG stage="+std::string(stage)+" shape="+dimensions+" shape_changed="+(changed?"1":"0")+
                " prepare_ms="+Milliseconds(started,runStart)+" run_ms="+Milliseconds(runStart,runEnd)+
                " output_check_ms="+Milliseconds(runEnd)+" output_elements="+std::to_string(n));
        }
        return std::move(result[0]);
    }

    void Sessions(int index) {
        lastDetShape.clear();lastRecShape.clear();deviceInfo="CPU";
        if(index>=0){
            ComPtr<IDXGIFactory1> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
            deviceInfo="DirectML adapter="+std::to_string(index);
            if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumAdapters1(index,&adapter))&&SUCCEEDED(adapter->GetDesc1(&desc))){
                char name[512]{};WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,512,nullptr,nullptr);
                deviceInfo+=" name="+std::string(name)+" vendor_id="+std::to_string(desc.VendorId)+" device_id="+std::to_string(desc.DeviceId)+
                    " dedicated_bytes="+std::to_string(desc.DedicatedVideoMemory)+" shared_bytes="+std::to_string(desc.SharedSystemMemory);
                LARGE_INTEGER driver{};
                if(SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver)))
                    deviceInfo+=" driver="+std::to_string(HIWORD(driver.HighPart))+"."+std::to_string(LOWORD(driver.HighPart))+"."+
                        std::to_string(HIWORD(driver.LowPart))+"."+std::to_string(LOWORD(driver.LowPart));
            }
        }
        if(diagnostics)log("DIAG stage=device "+deviceInfo+" threads="+std::to_string(threads));
        // 两个会话共享一个配置；请求串行，OpenCV 使用单线程避免嵌套线程池。
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(threads);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
        if (index >= 0) {
            options.DisableMemPattern();
            const OrtDmlApi* api = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
                reinterpret_cast<const void**>(&api)));
            Ort::ThrowOnError(api->SessionOptionsAppendExecutionProvider_DML(options, index));
        }
        auto sessionStart=DiagnosticClock::now();
        if(diagnostics)log("DIAG stage=session_det begin");
        det = std::make_unique<Ort::Session>(env, (root/L"det/inference.onnx").c_str(), options);
        if(diagnostics)log("DIAG stage=session_det ms="+Milliseconds(sessionStart));
        sessionStart=DiagnosticClock::now();
        if(diagnostics)log("DIAG stage=session_rec begin");
        rec = std::make_unique<Ort::Session>(env, (root/L"rec/inference.onnx").c_str(), options);
        if(diagnostics)log("DIAG stage=session_rec ms="+Milliseconds(sessionStart));
        for (auto* session : {det.get(), rec.get()}) {
            if (session->GetInputCount() != 1 || session->GetOutputCount() != 1)
                throw std::runtime_error("Unsupported model input/output count");
            const auto type = session->GetInputTypeInfo(0);
            auto info = type.GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetShape().size() != 4)
                throw std::runtime_error("Unsupported model input type/shape");
        }
        std::vector<float> d(3 * 320 * 320), r(3 * recHeight * recWidth);
        auto a = Run(*det, d, {1, 3, 320, 320},"warmup_det");
        auto b = Run(*rec, r, {1, 3, recHeight, recWidth},"warmup_rec");
        const auto recShape = b.GetTensorTypeAndShapeInfo().GetShape();
        if (a.GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t>{1, 1, 320, 320}
            || recShape.size() != 3 || recShape[0] != 1 || recShape[2] != static_cast<int64_t>(dictionary.size()))
            throw std::runtime_error("Model/dictionary shape mismatch");
        device = index;
        log("Device ready: " + (index < 0 ? std::string("CPU") : "DirectML adapter " + std::to_string(index)));
    }

    Impl(const fs::path& folder, const std::string& family, const std::string& tier,
         const std::string& mode, LogFunction logger,bool diagnosticEnabled) : root(folder), log(std::move(logger)),diagnostics(diagnosticEnabled) {
        auto verifyStart=DiagnosticClock::now();
        if(diagnostics)log("DIAG stage=model_verify begin model="+family+"/"+tier);
        if (mode != "auto" && mode != "cpu" && mode != "gpu") throw std::runtime_error("Invalid device mode");
        const auto entry = std::find_if(std::begin(kModels), std::end(kModels), [&](auto& m) { return family == m.family && tier == m.tier; });
        if (entry == std::end(kModels)) throw std::runtime_error("Unsupported model combination");
        // 检查全部文件后才创建任何推理会话；本程序不含下载路径。
        VerifyModelFile(root/L"profile.json", entry->profile);
        VerifyModelFile(root/L"det/inference.onnx", entry->det); VerifyModelFile(root/L"det/inference.yml", entry->det_config);
        VerifyModelFile(root/L"rec/inference.onnx", entry->rec); VerifyModelFile(root/L"rec/inference.yml", entry->rec_config);
        if(diagnostics)log("DIAG stage=model_verify ms="+Milliseconds(verifyStart));
        auto profileStart=DiagnosticClock::now();
        cv::FileStorage profile(ReadText(root/L"profile.json"), cv::FileStorage::READ | cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
        if (!profile.isOpened()) throw std::runtime_error("Cannot parse model profile");
        profile["mean"] >> mean; profile["std"] >> stddev; profile["dictionary"] >> dictionary;
        profile["threshold"] >> threshold; profile["box_threshold"] >> boxThreshold; profile["unclip"] >> unclip;
        profile["candidates"] >> candidates; profile["rec_height"] >> recHeight; profile["rec_width"] >> recWidth;
        if (mean.size() != 3 || stddev.size() != 3 || dictionary.size() < 2) throw std::runtime_error("Invalid profile");
        DWORD_PTR mask = 0, systemMask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &mask, &systemMask)) throw std::runtime_error("Cannot query CPU affinity");
        unsigned processors = 0; while (mask) { processors += static_cast<unsigned>(mask & 1); mask >>= 1; }
        threads = std::max(1u, processors / 2);
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (!GlobalMemoryStatusEx(&memory)) throw std::runtime_error("Cannot query memory");
        batch = static_cast<int>(std::clamp<ULONGLONG>(memory.ullAvailPhys / (1024ULL*1024*1024), 1, 8));
        cv::setNumThreads(1);
        log("Model " + family + "/" + tier + " threads=" + std::to_string(threads) + " batch_limit=" + std::to_string(batch));
        if(diagnostics)log("DIAG stage=profile_and_resources ms="+Milliseconds(profileStart)+" available_ram_bytes="+std::to_string(memory.ullAvailPhys));
        if (mode != "cpu") {
            ComPtr<IDXGIFactory1> factory;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                std::vector<std::pair<SIZE_T, int>> adapters;
                for (UINT i = 0;; ++i) {
                    ComPtr<IDXGIAdapter1> adapter;
                    auto hr = factory->EnumAdapters1(i, &adapter);
                    if (hr == DXGI_ERROR_NOT_FOUND) break;
                    if (FAILED(hr)) break;
                    DXGI_ADAPTER_DESC1 desc{};
                    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                        adapters.emplace_back(desc.DedicatedVideoMemory, static_cast<int>(i));
                }
                std::stable_sort(adapters.begin(), adapters.end(), [](auto a, auto b) { return a.first > b.first; });
                for (auto [bytes, index] : adapters) {
                    try {
                        Sessions(index);
                        ComPtr<IDXGIAdapter1> adapter;
                        ComPtr<ID3D12Device> gpu;
                        D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
                        const bool architectureKnown = SUCCEEDED(factory->EnumAdapters1(index, &adapter))
                            && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu)))
                            && SUCCEEDED(gpu->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture)));
                        // ponytail: UMA 沿用可用 RAM 批量；未知架构保守限流，长行仍受 128 MiB 输出预算保护。
                        batch = GpuBatchLimit(batch, bytes, architectureKnown && architecture.UMA);
                        log("GPU batch_limit="+std::to_string(batch)+" unified_memory="+
                            (architectureKnown ? (architecture.UMA ? "1" : "0") : "unknown"));
                        return;
                    }
                    catch (const std::exception& e) { det.reset(); rec.reset(); log("GPU initialization failed: " + std::string(e.what())); }
                }
            }
            if (mode == "gpu") throw std::runtime_error("No usable GPU for explicit development test");
            log("No usable GPU; falling back to CPU");
        }
        Sessions(-1);
    }

    std::vector<std::vector<cv::Point2f>> Detect(const cv::Mat& image) {
        auto prepareStart=DiagnosticClock::now();
        double ratio = std::min(1.0, 2048.0 / std::max(image.cols, image.rows));
        int h = std::max(32, static_cast<int>(std::nearbyint(image.rows * ratio / 32)) * 32);
        int w = std::max(32, static_cast<int>(std::nearbyint(image.cols * ratio / 32)) * 32);
        cv::Mat resized; cv::resize(image, resized, {w, h});
        std::vector<float> input(static_cast<size_t>(3) * h * w);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c)
            input[(static_cast<size_t>(c) * h + y) * w + x] = (resized.at<cv::Vec3b>(y,x)[c] / 255.f - mean[c]) / stddev[c];
        if(diagnostics)log("DIAG stage=det_preprocess ms="+Milliseconds(prepareStart)+" input="+std::to_string(w)+"x"+std::to_string(h));
        auto output = Run(*det, input, {1,3,h,w},"det_inference");
        auto postStart=DiagnosticClock::now();
        if (output.GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t>{1,1,h,w}) throw std::runtime_error("Invalid DB shape");
        cv::Mat pred(h,w,CV_32F,output.GetTensorMutableData<float>()), bitmap;
        cv::compare(pred, threshold, bitmap, cv::CMP_GT);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
        std::vector<std::vector<cv::Point2f>> boxes;
        for (size_t i = 0; i < std::min(contours.size(), static_cast<size_t>(candidates)); ++i) {
            std::vector<cv::Point2f> contour(contours[i].begin(), contours[i].end());
            float shortSide; auto box = Quad(contour, shortSide);
            if (shortSide < 3) continue;
            float minx = box[0].x, maxx = minx, miny = box[0].y, maxy = miny;
            for (auto p : box) { minx=std::min(minx,p.x); maxx=std::max(maxx,p.x); miny=std::min(miny,p.y); maxy=std::max(maxy,p.y); }
            int x0=std::clamp(static_cast<int>(std::floor(minx)),0,w-1), x1=std::clamp(static_cast<int>(std::ceil(maxx)),0,w-1);
            int y0=std::clamp(static_cast<int>(std::floor(miny)),0,h-1), y1=std::clamp(static_cast<int>(std::ceil(maxy)),0,h-1);
            cv::Mat mask(y1-y0+1,x1-x0+1,CV_8U,cv::Scalar(0));
            std::vector<cv::Point> polygon; for(auto p:box) polygon.emplace_back(static_cast<int>(p.x-x0),static_cast<int>(p.y-y0));
            cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{polygon}, cv::Scalar(1));
            if (cv::mean(pred(cv::Rect(x0,y0,x1-x0+1,y1-y0+1)),mask)[0] < boxThreshold) continue;
            const double length = cv::arcLength(box,true);
            if (length <= 0) continue;
            ClipperLib::Path path; for (auto p:box) path.emplace_back(static_cast<ClipperLib::cInt>(p.x),static_cast<ClipperLib::cInt>(p.y));
            ClipperLib::ClipperOffset offset; ClipperLib::Paths expanded;
            offset.AddPath(path,ClipperLib::jtRound,ClipperLib::etClosedPolygon);
            offset.Execute(expanded,cv::contourArea(box)*unclip/length);
            if (expanded.size()!=1) continue;
            contour.clear(); for(auto p:expanded[0]) contour.emplace_back(static_cast<float>(p.X),static_cast<float>(p.Y));
            box=Quad(contour,shortSide); if(shortSide<5)continue;
            for(auto& p:box){p.x=std::clamp(static_cast<float>(std::nearbyint(p.x*image.cols/w)),0.f,static_cast<float>(image.cols-1));
                p.y=std::clamp(static_cast<float>(std::nearbyint(p.y*image.rows/h)),0.f,static_cast<float>(image.rows-1));}
            if(cv::norm(box[0]-box[1])>3 && cv::norm(box[0]-box[3])>3) boxes.push_back(std::move(box));
        }
        std::stable_sort(boxes.begin(),boxes.end(),[](const auto& a,const auto& b){return a[0].y==b[0].y?a[0].x<b[0].x:a[0].y<b[0].y;});
        if(diagnostics)log("DIAG stage=det_postprocess ms="+Milliseconds(postStart)+" boxes="+std::to_string(boxes.size()));
        return boxes;
    }

    OcrResult RecognizeImage(const cv::Mat& image) {
        auto boxes=Detect(image);
        auto cropStart=DiagnosticClock::now();
        OcrResult result{image.cols,image.rows,{}}; result.items.resize(boxes.size());
        std::vector<cv::Mat> crops;
        for(size_t i=0;i<boxes.size();++i){
            const auto& b=boxes[i];
            int w=static_cast<int>(std::max(cv::norm(b[0]-b[1]),cv::norm(b[2]-b[3])));
            int h=static_cast<int>(std::max(cv::norm(b[0]-b[3]),cv::norm(b[1]-b[2])));
            std::vector<cv::Point2f> dest{{0,0},{static_cast<float>(w),0},{static_cast<float>(w),static_cast<float>(h)},{0,static_cast<float>(h)}};
            cv::Mat crop; cv::warpPerspective(image,crop,cv::getPerspectiveTransform(b,dest),{w,h},cv::INTER_CUBIC,cv::BORDER_REPLICATE);
            if(static_cast<double>(h)/w>=1.5)cv::rotate(crop,crop,cv::ROTATE_90_COUNTERCLOCKWISE);
            crops.push_back(std::move(crop));
            result.items[i].quad=b;
            result.items[i].rect=cv::boundingRect(b)&cv::Rect(0,0,image.cols,image.rows);
        }
        std::vector<size_t> order(crops.size());std::iota(order.begin(),order.end(),0);
        auto ratio=[&](size_t i){return static_cast<double>(crops[i].cols)/crops[i].rows;};
        std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return ratio(a)<ratio(b);});
        if(diagnostics)log("DIAG stage=crop_and_sort ms="+Milliseconds(cropStart)+" batch_limit="+std::to_string(batch));
        for(size_t start=0;start<order.size();){
            auto batchStart=DiagnosticClock::now();
            size_t count=std::min(static_cast<size_t>(batch),order.size()-start);
            int width=std::max(recWidth,static_cast<int>(recHeight*ratio(order[start+count-1])));
            if(width>8192)throw std::runtime_error("Text line exceeds supported aspect ratio");
            // 限制 CTC 输出缓存约 128 MiB，宽行自动缩小批量，不截断文本。
            size_t perLine=static_cast<size_t>((width+7)/8)*dictionary.size()*sizeof(float);
            count=std::min(count,std::max(size_t{1},size_t{128*1024*1024}/std::max(size_t{1},perLine)));
            width=std::max(recWidth,static_cast<int>(recHeight*ratio(order[start+count-1])));
            std::vector<float> input(count*3*recHeight*width,0);
            for(size_t j=0;j<count;++j){
                const auto& crop=crops[order[start+j]]; int cw=std::min(width,static_cast<int>(std::ceil(recHeight*ratio(order[start+j]))));
                cv::Mat resized;cv::resize(crop,resized,{cw,recHeight});
                for(int y=0;y<recHeight;++y)for(int x=0;x<cw;++x)for(int c=0;c<3;++c)
                    input[((j*3+c)*recHeight+y)*width+x]=(resized.at<cv::Vec3b>(y,x)[c]/255.f-.5f)/.5f;
            }
            if(diagnostics)log("DIAG stage=rec_preprocess ms="+Milliseconds(batchStart)+" line_start="+std::to_string(start)+" batch="+std::to_string(count)+" width="+std::to_string(width));
            auto output=Run(*rec,input,{static_cast<int64_t>(count),3,recHeight,width},"rec_inference");
            auto decodeStart=DiagnosticClock::now();
            auto shape=output.GetTensorTypeAndShapeInfo().GetShape();
            if(shape.size()!=3||shape[0]!=static_cast<int64_t>(count)||shape[2]!=static_cast<int64_t>(dictionary.size()))throw std::runtime_error("Invalid CTC shape");
            const auto* data=output.GetTensorData<float>();
            for(size_t j=0;j<count;++j){auto& item=result.items[order[start+j]];size_t last=0,chars=0;float score=0;
                for(int64_t t=0;t<shape[1];++t){auto row=data+(j*shape[1]+t)*dictionary.size();size_t id=std::max_element(row,row+dictionary.size())-row;
                    if(id && id!=last){item.text+=dictionary[id];score+=row[id];++chars;}last=id;}
                item.confidence=chars?score/static_cast<float>(chars):0;
            }
            if(diagnostics)log("DIAG stage=ctc_decode ms="+Milliseconds(decodeStart)+" line_start="+std::to_string(start)+" batch="+std::to_string(count));
            start+=count;
        }
        return result;
    }
};

Ocr::Ocr(const fs::path& p,const std::string& family,const std::string& tier,const std::string& device,LogFunction log,bool diagnostics)
    :impl_(std::make_unique<Impl>(p,family,tier,device,std::move(log),diagnostics)){}
Ocr::~Ocr()=default;
void Ocr::SetDiagnostics(bool enabled){
    if(enabled&&!impl_->diagnostics)impl_->log("DIAG stage=device "+impl_->deviceInfo+" threads="+std::to_string(impl_->threads));
    impl_->diagnostics=enabled;
}
OcrResult Ocr::Recognize(const std::vector<unsigned char>& bytes){
    auto decodeStart=DiagnosticClock::now();
    // 首版只承诺 PNG，解码前限制 IHDR 尺寸，避免畸形输入导致巨量分配。
    const unsigned char signature[]{137,80,78,71,13,10,26,10};
    if(bytes.size()<33||bytes.size()>64*1024*1024||!std::equal(std::begin(signature),std::end(signature),bytes.begin())
        ||std::string(bytes.begin()+12,bytes.begin()+16)!="IHDR")throw std::runtime_error("Invalid PNG input");
    auto be=[&](size_t i){return (uint32_t(bytes[i])<<24)|(uint32_t(bytes[i+1])<<16)|(uint32_t(bytes[i+2])<<8)|bytes[i+3];};
    auto w=be(16),h=be(20);
    if(!w||!h||w>16384||h>16384||uint64_t(w)*h>64*1024*1024)throw std::runtime_error("PNG dimensions exceed limit");
    auto image=cv::imdecode(bytes,cv::IMREAD_COLOR|cv::IMREAD_IGNORE_ORIENTATION);
    if(image.empty()||image.cols!=static_cast<int>(w)||image.rows!=static_cast<int>(h))throw std::runtime_error("PNG decode failed");
    if(impl_->diagnostics)impl_->log("DIAG stage=png_decode ms="+Milliseconds(decodeStart)+" image="+std::to_string(w)+"x"+std::to_string(h)+" bytes="+std::to_string(bytes.size())+" device="+std::to_string(impl_->device));
    try{return impl_->RecognizeImage(image);}
    catch(const Ort::Exception& e){
        if(impl_->device<0)throw;
        impl_->log("GPU inference failed; retrying on CPU: "+std::string(e.what()));
        impl_->det.reset();impl_->rec.reset();impl_->Sessions(-1);
        return impl_->RecognizeImage(image);
    }
}
