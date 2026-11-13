#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include "model_hashes.hpp"
namespace fs = std::filesystem;
struct Selection { std::wstring family=L"PP-OCRv6", tier=L"small"; bool diagnostics=false; };
struct PluginPaths { fs::path install, config; };
const ModelHashes& FindModel(const Selection& selection);
Selection ReadSelection(const PluginPaths& paths);
void SaveSelection(const PluginPaths& paths, const Selection& selection);
fs::path DownloadRoot(const PluginPaths& paths, const ModelHashes& model);
void VerifyPair(const fs::path& root, const ModelHashes& model);
fs::path ResolveModel(const PluginPaths& paths, const ModelHashes& model);
void PluginLog(const std::string& message) noexcept;
int ShowSettings(const PluginPaths& paths, HINSTANCE instance);
