# 归集已构建的 Release worker；x86 DLL 由 stage_plugin.ps1 单独归集。
param(
    [string]$VcRuntime = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$destination = Join-Path $root 'dist/worker'
$sources = @(
    'build/Release/PaddleOcrWorker.exe',
    'third_party/ort-directml-1.24.4/runtimes/win-x64/native/onnxruntime.dll',
    'third_party/ort-directml-1.24.4/runtimes/win-x64/native/onnxruntime_providers_shared.dll',
    'third_party/directml-1.15.4/bin/x64-win/DirectML.dll',
    'third_party/opencv-5.0.0/bin/opencv_world500.dll'
) | ForEach-Object { Join-Path $root $_ }
$sources += @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','concrt140.dll') | ForEach-Object { Join-Path $VcRuntime $_ }
foreach ($source in $sources) { if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing dependency: $source" } }
New-Item -ItemType Directory -Force $destination | Out-Null
foreach ($source in $sources) { Copy-Item -LiteralPath $source -Destination $destination -Force }
# 升级后不把旧版推理图像库继续打进发布包；只删除此脚本曾归集的明确文件。
$obsolete = [IO.Path]::GetFullPath((Join-Path $destination 'opencv_world4100.dll'))
if ([IO.Path]::GetDirectoryName($obsolete) -ne [IO.Path]::GetFullPath($destination)) { throw 'Invalid obsolete runtime path' }
if (Test-Path -LiteralPath $obsolete) { Remove-Item -LiteralPath $obsolete }
& (Join-Path $PSScriptRoot 'stage_licenses.ps1')
$manifest = Get-Content (Join-Path $root 'models.lock.json') -Raw | ConvertFrom-Json
$manifest.models = @($manifest.models | Where-Object { $_.release_enabled })
$manifest | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $root 'dist/models.lock.json') -Encoding utf8
# 所有档位的轻量配置随插件提供；设置下载时把配套 profile.json 复制到完整组合临时目录。
foreach ($model in ($manifest.models | Where-Object { $_.role -eq 'det' })) {
    $relative = $model.family + '/' + $model.tier
    $profile = Join-Path $root ('dist/models/' + $relative + '/profile.json')
    if (!(Test-Path -LiteralPath $profile)) { $profile = Join-Path $root ('downloads/models/' + $relative + '/profile.json') }
    $target = Join-Path $root ('dist/profiles/' + $relative + '.json')
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $profile -Destination $target -Force
}
# EXE/DLL 在归集后签名，不生成签名前的文件哈希清单。
$obsoleteManifest=Join-Path $root 'dist/worker-files.json'
if(Test-Path -LiteralPath $obsoleteManifest){Remove-Item -LiteralPath $obsoleteManifest}
Write-Output 'Staged Release worker and runtime files. Final acceptance remains separate.'
