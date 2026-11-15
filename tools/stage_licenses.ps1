# 合并项目及第三方许可，保留原文，仅合并完全相同的文本。
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$licenses = [ordered]@{
    'opencv/LICENSE'='third_party/opencv-5.0.0/LICENSE'
    'paddleocr/LICENSE'='third_party/paddleocr-3.7.0/LICENSE'
    'clipper/LICENSE.txt'='third_party/clipper/License.txt'
    'onnxruntime/LICENSE'='third_party/ort-directml-1.24.4/LICENSE'
    'onnxruntime/ThirdPartyNotices.txt'='third_party/ort-directml-1.24.4/ThirdPartyNotices.txt'
    'directml/LICENSE.txt'='third_party/directml-1.15.4/LICENSE.txt'
    'directml/LICENSE-CODE.txt'='third_party/directml-1.15.4/LICENSE-CODE.txt'
    'directml/ThirdPartyNotices.txt'='third_party/directml-1.15.4/ThirdPartyNotices.txt'
}
$groups=[ordered]@{}
foreach($entry in $licenses.GetEnumerator()){
    $text=[IO.File]::ReadAllText((Join-Path $root $entry.Value)).Replace("`r`n","`n").Trim()
    if(!$groups.Contains($text)){$groups[$text]=@()}
    $groups[$text]+=$entry.Key
}
$notices=Join-Path $root 'third_party/opencv-5.0.0/etc/licenses'
foreach($file in (Get-ChildItem -LiteralPath $notices -File | Sort-Object Name)){
    # Release 已关闭 IPP；旧安装目录可能残留以前的 IPP 声明。
    if($file.Name -like 'ipp*'){continue}
    $text=[IO.File]::ReadAllText($file.FullName).Replace("`r`n","`n").Trim()
    if(!$groups.Contains($text)){$groups[$text]=@()}
    $groups[$text]+='opencv/'+$file.Name
}
$parts=@('百度飞桨：项目原创代码采用 MIT；第三方库及模型适用各自许可，以下保留其授权及声明。',
    [IO.File]::ReadAllText((Join-Path $root 'LICENSE')).Trim())
foreach($text in $groups.Keys){$parts+= "`n======== " + ($groups[$text] -join ' / ') + " ========`n" + $text}
$dist=[IO.Path]::GetFullPath((Join-Path $root 'dist'))
New-Item -ItemType Directory -Force $dist | Out-Null
[IO.File]::WriteAllText((Join-Path $dist 'LICENSE.txt'),($parts -join "`r`n`r`n"),[Text.UTF8Encoding]::new($false))
# 仅在合并成功后删除已由本脚本替代的旧许可证目录。
$old=[IO.Path]::GetFullPath((Join-Path $dist 'licenses'))
if([IO.Path]::GetDirectoryName($old) -ne $dist){throw 'Invalid old license directory'}
if(Test-Path -LiteralPath $old){
    if((Get-Item -LiteralPath $old).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Unexpected license directory link'}
    if(Get-ChildItem -LiteralPath $old -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}){throw 'Unexpected license entry link'}
    Remove-Item -LiteralPath $old -Recurse -Force
}
Write-Output 'Consolidated project and third-party licenses into dist/LICENSE.txt.'