# 开发期获取已固定版本和哈希的原生依赖，不在插件运行时调用。
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$packages = @(
    @{
        Name = 'ort-directml-1.24.4'
        Url = 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg'
        Hash = '57E9F11B73437BEF7A309496135D4C1F96B1A8E9DDBA60013FA27BFC1D788681'
    },
    @{
        Name = 'directml-1.15.4'
        Url = 'https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg'
        Hash = '4E7CB7DDCE8CF837A7A75DC029209B520CA0101470FCDF275C1F49736A3615B9'
    }
)
New-Item -ItemType Directory -Force (Join-Path $root 'downloads') | Out-Null
foreach ($package in $packages) {
    $archive = Join-Path $root "downloads/$($package.Name).zip"
    if (!(Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest $package.Url -OutFile "$archive.part" -TimeoutSec 120
        if ((Get-FileHash -LiteralPath "$archive.part" -Algorithm SHA256).Hash -ne $package.Hash) {
            throw "Checksum mismatch: $($package.Name)"
        }
        Move-Item -LiteralPath "$archive.part" -Destination $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $package.Hash) {
        throw "Checksum mismatch: $archive"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $root "third_party/$($package.Name)") -Force
    Write-Output "Verified $($package.Name)"
}
