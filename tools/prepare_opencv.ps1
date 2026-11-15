# VS2022 x64 源码构建固定 OpenCV 5.0.0，Release /MD、Debug /MDd。
# 上游 tag 5.0.0 -> 40738fb16ceddb5fb3fea747585f7ce6abb0605b。
param([ValidateSet('Release','Debug','Both')][string]$Configuration='Both')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$archive=Join-Path $root 'downloads/opencv-5.0.0.zip'
$source=Join-Path $root 'third_party/src/opencv-5.0.0'
$build=Join-Path $root 'build/opencv-5.0.0-msvc'
$cmake='C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if(!(Test-Path -LiteralPath $cmake)){throw 'VS2022 bundled CMake is required'}
$install=Join-Path $root 'third_party/opencv-5.0.0'
$expected='E2ABAA1EA2443FB163299369506521518EE02D8E51B88A144A128C4E492123BD'
New-Item -ItemType Directory -Force (Split-Path $archive),$build | Out-Null
if(!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest 'https://github.com/opencv/opencv/archive/refs/tags/5.0.0.zip' -OutFile "$archive.part" -TimeoutSec 180
    if((Get-FileHash -LiteralPath "$archive.part").Hash -ne $expected){throw 'OpenCV source SHA256 mismatch'}
    Move-Item -LiteralPath "$archive.part" -Destination $archive
}
if((Get-FileHash -LiteralPath $archive).Hash -ne $expected){throw 'OpenCV source SHA256 mismatch'}
if(!(Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt'))){Expand-Archive -LiteralPath $archive -DestinationPath (Split-Path $source)}
# PNG 是已承诺的输入格式；关闭 IPP 缩小分发体积，已验证四种模型结果及耗时。
& $cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 `
    '-DCMAKE_ASM_COMPILER:FILEPATH=NOTFOUND' `
    "-DCMAKE_INSTALL_PREFIX=$install" '-DOPENCV_BIN_INSTALL_PATH=bin' '-DOPENCV_LIB_INSTALL_PATH=lib' '-DOPENCV_INCLUDE_INSTALL_PATH=include' `
    '-DCMAKE_CXX_STANDARD=17' '-DBUILD_WITH_STATIC_CRT=OFF' '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL' `
    '-DBUILD_LIST=core,imgproc,imgcodecs,geometry,world' '-DBUILD_opencv_world=ON' '-DBUILD_SHARED_LIBS=ON' `
    '-DBUILD_TESTS=OFF' '-DBUILD_PERF_TESTS=OFF' '-DBUILD_EXAMPLES=OFF' '-DBUILD_opencv_apps=OFF' '-DBUILD_JAVA=OFF' '-DBUILD_opencv_highgui=OFF' `
    '-DWITH_IPP=OFF' '-DWITH_OPENCL=OFF' '-DWITH_ITT=OFF' '-DWITH_LAPACK=OFF' '-DWITH_EIGEN=OFF' `
    '-DWITH_PNG=ON' '-DBUILD_PNG=ON' '-DBUILD_ZLIB=ON' '-DWITH_JPEG=OFF' '-DWITH_TIFF=OFF' '-DWITH_WEBP=OFF' `
    '-DWITH_OPENJPEG=OFF' '-DWITH_JASPER=OFF' '-DWITH_OPENEXR=OFF' '-DWITH_AVIF=OFF' '-DWITH_GIF=OFF' '-DWITH_FFMPEG=OFF' '-DWITH_UNIFONT=OFF' `
    '-DWITH_GSTREAMER=OFF' '-DWITH_VTK=OFF' '-DWITH_PROTOBUF=OFF' '-DOPENCV_DNN_OPENVINO=OFF' `
    '-DBUILD_DOCS=OFF' '-DOPENCV_ENABLE_NONFREE=OFF' *> (Join-Path $build 'configure.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $build 'configure.log') -Tail 60;throw 'OpenCV configure failed'}
$configs=if($Configuration -eq 'Both'){@('Release','Debug')}else{@($Configuration)}
foreach($config in $configs){
    Write-Output "Building OpenCV $config; log: $build/build-$config.log"
    & $cmake --build $build --config $config --target INSTALL --parallel 4 -- /p:CL_MPCount=4 *> (Join-Path $build "build-$config.log")
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $build "build-$config.log") -Tail 60;throw "OpenCV $config build failed"}
}
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $install 'LICENSE') -Force
Copy-Item -LiteralPath (Join-Path $build 'configure.log') -Destination (Join-Path $install 'build-configuration.log') -Force
Write-Output "OpenCV 5.0.0 installed: $install"
