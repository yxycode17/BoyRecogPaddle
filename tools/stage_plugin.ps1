# x86 CRT 由读屏宿主及其安装的 VC 运行库提供；x64 worker 仍独立携带 CRT。
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$destination=Join-Path $root 'dist'
$sources=@((Join-Path $root 'build-x86/Release/BoyRecogPaddle.dll'))
foreach($file in $sources){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing dependency: $file"}}
foreach($file in $sources){Copy-Item -LiteralPath $file -Destination $destination -Force}
# 仅清理旧脚本曾归集到根目录的三个重复文件，不触碰 worker 子目录。
foreach($name in @('msvcp140.dll','vcruntime140.dll','concrt140.dll')){
    $obsolete=[IO.Path]::GetFullPath((Join-Path $destination $name))
    if([IO.Path]::GetDirectoryName($obsolete) -ne [IO.Path]::GetFullPath($destination)){throw 'Invalid obsolete CRT path'}
    if(Test-Path -LiteralPath $obsolete){Remove-Item -LiteralPath $obsolete -ErrorAction Stop}
}
# DLL 在归集后签名，不生成签名前的文件哈希清单。
$obsoleteManifest=Join-Path $destination 'plugin-files.json'
if(Test-Path -LiteralPath $obsoleteManifest){Remove-Item -LiteralPath $obsoleteManifest}
Write-Output 'Staged x86 plugin; host supplies x86 CRT. Use stage_worker.ps1 for bundled x64 dependencies.'
