# 百度飞桨

基于 PaddleOCR 的保益悦听本地文字识别插件，插件版本为 1.1.0，唯一引擎名称为 `PaddleOCR`。

## 功能

- 支持 PP-OCRv6 tiny/small/medium、PP-OCRv5 mobile，默认随包提供 v6 small。
- 优先使用 ONNX Runtime DirectML，在兼容的 NVIDIA、AMD、Intel 显卡（含核显）上推理；GPU 不可用或运行失败时回退 CPU。
- 原生 Win32 设置窗口支持模型选择、下载进度、取消和诊断日志开关，支持键盘操作与读屏访问。
- 普通识别离线执行，不临时下载模型；首次加载后复用模型，不逐次计算哈希。
- 接受 PNG 编码图片，返回文字和原图坐标。当前能力标志为 `39`，不提供非文字类型检测、图片描述或单图标语义识别。

当前尚未接入官方方向分类模型，不能保证任意旋转图片的识别效果；完整干净环境验收仍待完成。

## 架构与接口

宿主加载 x86 `BoyRecogPaddle.dll`。插件在首次有效识别时后台启动 x64 `PaddleOcrWorker.exe`，通过命名管道通信；管道断开时 worker 自动退出。DLL 不加载 OpenCV、ONNX Runtime 或 DirectML，推理依赖只属于 worker。

协作开发请先阅读 [保益识别插件开发说明](docs/保益识别插件开发说明.md)，导出声明见 `src/BoyRecogPlugin.h`。文档副本同步于 2026-11-13，宿主接口更新时应同步更新。

## 构建

需要 Windows x64、Visual Studio 2022 C++ 桌面开发工具、Windows SDK、CMake 3.24 或以上及 Python 3.11 或以上。Release 使用 `/MD`，Debug 使用 `/MDd`。

在仓库根目录运行 PowerShell：

```powershell
python -m venv .venv
.venv/Scripts/python.exe -m pip install PyYAML==6.0.3
./tools/prepare_dependencies.ps1
./tools/prepare_opencv.ps1 -Configuration Release
.venv/Scripts/python.exe tools/prepare_models.py
.venv/Scripts/python.exe tools/prepare_models.py --family PP-OCRv6 --tier tiny --output downloads/models
.venv/Scripts/python.exe tools/prepare_models.py --family PP-OCRv6 --tier medium --output downloads/models
.venv/Scripts/python.exe tools/prepare_models.py --family PP-OCRv5 --tier mobile --output downloads/models
```

依赖脚本获取固定版本 ONNX Runtime DirectML 1.24.4、DirectML 1.15.4 和 OpenCV 5.0.0。构建期需准备四种模型组合以生成配置与校验数据，最终只随包分发 small。

还需准备 Clipper 6.4.2 的 `cpp/clipper.cpp`、`cpp/clipper.hpp` 和 `License.txt`，放入 `third_party/clipper/`；打包前将 PaddleOCR v3.7.0 的 `LICENSE` 放到 `third_party/paddleocr-3.7.0/`。第三方源码、权重和二进制不在本仓库中。

`prepare_opencv.ps1` 当前使用 VS2022 Community 默认安装路径，其他安装位置需调整脚本中的 `$cmake`。以下命令要求 CMake 已在 PATH 中：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32
cmake --build build-x86 --config Release
```

测试与基准工具位于 `tools/`，包括宿主 ABI、模型验证、设置、管道生命周期、诊断开关和旋转坐标测试。Python 性能测试所需依赖列在 `tools/requirements-benchmark.txt` 中；测试图片由开发者自行提供。

## 发布约定

版本号必须同步维护：`src/plugin.rc`（插件 DLL）、`src/worker.rc`（worker EXE）的 `FILEVERSION`、`PRODUCTVERSION`、`FileVersion`、`ProductVersion`，以及发布包 `dist/info.json` 的 `version`。当前字符串版本为 `1.1.0`，数值版本为 `1,1,0,0`，包版本为 `v1.1.0`；后续升级必须表达同一个版本，不能只更新其中一处。发布前检查实际 DLL/EXE 的版本资源与包元数据是否一致。

**所有最终分发文件必须归集到 `dist/`，发布时直接打包该目录内容。** 不得要求终端用户从 build、third_party 或开发机补文件。`dist/` 仅跟踪商店元数据 `info.json`，其余发布文件保留在本地，不纳入 Git。

```powershell
./tools/stage_worker.ps1
./tools/stage_plugin.ps1
```

`stage_worker.ps1` 的 `-VcRuntime` 参数可指定本机 x64 Release CRT 目录。最终目录应包括：

```text
dist/
  BoyRecogPaddle.dll
  info.json
  发布说明.md
  worker/                 x64 EXE、ORT、DirectML、OpenCV 和 x64 CRT
  models/PP-OCRv6/small/   默认模型、字典和配置
  profiles/               可选模型的配套配置
  LICENSE.txt             项目 MIT 与第三方及模型许可合并文件
  models.lock.json
```

商店元数据 `info.json` 随仓库维护；归集脚本不生成发布说明，打包者需另行准备。x86 DLL 的 CRT 由读屏宿主及其安装程序提供；x64 worker 的 Release CRT 随包分发。终端不需要 Python 或 Visual Studio。打包前验证依赖、许可证及默认模型离线可用性。

## 配置和日志

配置保存到实际 Windows 文档目录下的 `BoyPcReader/plugins/Recognition/<插件安装文件夹名>/`，包含 `settings.ini`、下载的 `models/` 和 `logs/`。保持安装文件夹名不变即可在升级后复用配置与下载模型。

模型清单随插件更新。选择尚未下载的模型时询问是否下载；拒绝或取消则保留原设置。识别优先加载配置目录中经过验证的所选模型组合，其次加载安装目录的同版本组合。

诊断开关默认关闭，下一次识别生效且不重新加载模型。开启后 `plugin.log` 和 `worker.log` 中的 `DIAG` 记录提供设备与分阶段耗时，不记录图片内容或识别文字。每个程序保留当前日志和一份 `previous.log`；`.lock` 是日志占用锁，无需手工清理。

## 参与开发

可通过 Fork 和 Pull Request 提交修改。修改识别流程时请验证 CPU/GPU 结果、原图坐标和模型复用；修改设置或下载流程时请验证取消、失败和旧配置保留。不要提交模型、运行库、用户配置、日志或测试图片。

## 授权

项目原创代码采用 MIT，见根目录 LICENSE。第三方库及模型沿用各自许可；tools/stage_licenses.ps1 将完整条款与声明合并为 dist/LICENSE.txt，相同文本只保留一份，不将第三方内容重新授权为 MIT。
