# x64 worker 实现与协议

当前实现是原生 det/rec worker，支持已启用的四套模型，已接入 x86 宿主 DLL 和 Win32 设置/下载窗口，尚未接入官方整页/文本行方向分类模型。后续方向校正必须保持原图坐标，不能将当前竖长文本裁切旋转等同于完整方向识别。

## 构建和依赖

```powershell
./tools/prepare_opencv.ps1
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
cmake --build build --config Release --target PaddleOcrWorker
cmake --build build --config Debug --target PaddleOcrWorker
.venv/Scripts/python.exe tools/test_native_worker.py 'D:\无标题.png'
.venv/Scripts/python.exe tools/benchmark_native_worker.py 'D:\无标题.png'
./tools/stage_worker.ps1
```

Release `/MD`、Debug `/MDd`。Python/PyYAML 仅用于构建：`generate_native_profiles.py` 验证四套模型文件及官方配置，生成每档 profile.json 和编译期哈希表；worker 不需要 Python、YAML 库或网络。首次构建前显式准备四套模型，构建失败不会自动下载。

OpenCV 已切换为从官方固定源码构建的 5.0.0，不再复用旧工程的 4.10 二进制。源码 tag 对应提交 `40738fb16ceddb5fb3fea747585f7ce6abb0605b`，ZIP SHA256 固定在准备脚本中；使用 VS2022 自带 CMake，禁用误探测 PATH 中 MinGW 的 ASM 路径，Release/Debug 分别链接 `/MD`、`/MDd`。构建 core、imgproc、imgcodecs、geometry、world，以及 geometry 依赖 flann，保留 IPP；PNG/libpng/zlib 及 IPP/IW 等随源码安装的第三方声明统一归集到 dist/licenses/opencv。当前仅承诺 PNG 输入，关闭不需要的编解码器、GUI 和 Unicode 字体。Clipper 6.4.2 来自此前已校验的 Paddle 发布归档。ORT/DirectML 来源和版本见准备脚本。最终干净环境完整依赖验收仍待完成，当前 staging 不等于可发布认证。

旧构建目录迁移时显式执行 `cmake -S . -B build -DOPENCV_DIR=D:/projects/codex/BoyRecogPaddle/third_party/opencv-5.0.0`，避免继续使用缓存的旧 OpenCV 路径；路径根据本机检出目录调整。当前运行库名为 `opencv_world500.dll`，Debug 为 `opencv_world500d.dll`，主程序的延迟导入及绝对路径加载保持版本一致。

worker 对 ORT、OpenCV 使用延迟导入，进入 main 后限制 DLL 搜索，并按 EXE 目录绝对路径加载固定版本运行库。x86 插件 DLL 不得链接这些库。`dist/worker` 存放 EXE 和 x64 依赖，`dist/profiles` 提供所有四档的小型配置；设置下载器将对应 profile.json 复制到该组合的临时目录，随权重整套校验后发布。worker 对 profile、det/rec 权重和官方 YAML 逐一验证编译期 SHA256，模型缺失或损坏直接失败，不联网、不换档。

## 启动

生产调用：`PaddleOcrWorker.exe --pipe <名称> --parent-pid <宿主PID> --model-dir <绝对目录> --family PP-OCRv6 --tier small`。

- 管道由 DLL 创建，worker 作为客户端连接，名称须以 `\\.\pipe\BoyRecogPaddle-` 开头，5 秒连接超时。DLL 使用 GUID 唯一名称、当前用户 ACL、首实例限制、拒绝远程访问并核对客户端 PID。以 CREATE_NO_WINDOW | CREATE_SUSPENDED 启动 worker，加入 kill-on-close Job Object 后恢复执行，不显示控制台窗口。DLL 连接等待最长 10 秒，整个 OCR 管道往返最长 125 秒，超时关闭连接并结束 worker。
- worker 核查管道服务端 PID 和当前用户令牌。每个已开始的帧/请求最长 120 秒，独立线程每 50 ms 检查管道、父进程和请求时限，断管或父进程退出时直接结束进程，即使正在模型加载、ORT 推理或结果写入。空闲期间可继续复用，不周期退出。
- 模型在首次 OCR 帧时加载，ping 不加载。GPU 优先，按独立显存排序枚举硬件适配器，逐个创建并试跑；全部失败才使用 CPU。GPU Run 异常后释放会话，在同档 CPU 重试一次。当前未做多 GPU 性能竞赛或线程数基准调优，资源参数由 CPU affinity、可用内存及显存约束计算。
- 默认配置根目录通过 Documents Known Folder 和 EXE 上一级安装目录名计算；`--config-dir <绝对路径>` 用于 DLL 明确传递路径或开发测试。worker.log/worker.previous.log 每次启动轮换覆盖；并发占用通过锁文件和实例子目录隔离。
- `--device cpu|gpu` 供开发对照及明确故障恢复；正常生产使用默认 auto，不能根据 CPU 测速改变 GPU 优先策略。
- 开发入口 `--image <PNG路径> --output <JSON路径>` 不走管道，可输出文字、置信度、原图四点与矩形以检查算法；这些开发输出不是生产日志。

## 帧格式

所有整数为显式编码的 32 位无符号小端整数，头部 16 字节，无 C 结构体内存直传。magic `0x31525042` 表示 `BPR1`，末尾版本 1。头部依次为 magic、operation、request_id、payload_size。

| operation | 请求负载 | 响应 |
|---|---|---|
| 1 | PNG 编码字节 | OCR 项目 |
| 2 | 空 | ping，成功但零项目，不加载模型 |
| 3 | 空 | 成功但零项目，随后退出 |

响应 operation 为请求值 OR `0x80000000`，request_id 原样返回。响应负载依次为 status、原图 width、height、item_count，然后是项目数组，最后为 error_utf8_length + 错误 UTF-8 字节。成功 status=0、错误文本为空；失败 status=1、项目数为零，错误内容不能当作 OCR 文本。

每个项目依次为 type（当前固定 3）、left、top、right、bottom、text_utf8_length、UTF-8 字节。字符串无终止 NUL；DLL 转 UTF-16 后按宿主 ABI 交付。矩形均在输入原图范围内，right/bottom 不包含边界。不伪造字符框，DLL 使用空字符数组。

图片编码上限 64 MiB，PNG 单边不超过 16384、像素不超过 64 Mi；检测输入长边上限 2048、按 32 对齐，识别高度按配置、动态宽度不超过 8192。宽行自适应缩小批量，CTC 输出缓存预算约 128 MiB；不裁掉超过宽度上限的文字，而是明确失败。响应上限 8 MiB。非法 magic、操作码或长度直接关闭连接；普通识别错误返回失败帧且保留连接。

当前文字项按检测框顶点从上到下、同高度从左到右排序，复杂多栏阅读顺序仍需宿主验收。PNG 之外的格式当前明确拒绝。
