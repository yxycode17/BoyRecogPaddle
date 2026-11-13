#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

struct TextItem {
    std::string text;
    cv::Rect rect;
    std::vector<cv::Point2f> quad;
    float confidence = 0;
};
struct OcrResult { int width = 0, height = 0; std::vector<TextItem> items; };
using LogFunction = std::function<void(const std::string&)>;

class Ocr {
public:
    Ocr(const std::filesystem::path& root, const std::string& family, const std::string& tier,
        const std::string& device, LogFunction log, bool diagnostics=false);
    ~Ocr();
    OcrResult Recognize(const std::vector<unsigned char>& png);
    void SetDiagnostics(bool enabled);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
