param(
 [string]$DependencyPrefix = '',
 [ValidateSet('c','cpp','java','go','rust','csharp','python')]
 [string[]]$Languages = @('c','cpp','java','go','rust','csharp','python')
)
$ErrorActionPreference = 'Stop'
# 每个语言在自己的目录中构建；仅把最终发行文件写入 bin，不混用中间产物。
if ('python' -in $Languages) {
 # Python 直接运行源码；检查最低版本与语法，不安装第三方运行时依赖。
 & python -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
 if ($LASTEXITCODE -ne 0) { throw 'Python 3.10+ required' }
 & python -m compileall -q (Join-Path $PSScriptRoot 'python/embedai') (Join-Path $PSScriptRoot 'python/console.py')
 if ($LASTEXITCODE -ne 0) { throw 'Python syntax check failed' }
}
if ('c' -in $Languages) {
 $cPrefix = $DependencyPrefix
 if (-not $cPrefix -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'deps/windows-x64'))) {
  $cPrefix = Join-Path $PSScriptRoot 'deps/windows-x64'
 }
 $cArgs = @('-S', (Join-Path $PSScriptRoot 'c'), '-B', (Join-Path $PSScriptRoot 'build/c'))
 if ($cPrefix) { $cArgs += "-DCMAKE_PREFIX_PATH=$cPrefix" }
 & cmake @cArgs
 if ($LASTEXITCODE -ne 0) { throw 'C configure failed' }
 & cmake --build (Join-Path $PSScriptRoot 'build/c') --config Release
 if ($LASTEXITCODE -ne 0) { throw 'C build failed' }
 $cRelease = Join-Path $PSScriptRoot 'build/c/Release'
 if (-not (Test-Path -LiteralPath (Join-Path $cRelease 'EmbedAiChatC.exe'))) { $cRelease = Join-Path $PSScriptRoot 'build/c' }
 New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'bin/c') | Out-Null
 foreach ($artifact in @('EmbedAiChatC.exe','embed_ai_chat_c.lib','embed_ai_chat_c_curl.lib','embed_ai_chat_c_mcp.lib','EmbedAiChatCTests.exe')) {
  $source = Join-Path $cRelease $artifact
  if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $PSScriptRoot "bin/c/$artifact") -Force }
 }
}
if ('cpp' -in $Languages) {
 if (-not $DependencyPrefix -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'deps/windows-x64'))) {
  $DependencyPrefix = Join-Path $PSScriptRoot 'deps/windows-x64'
 }
 $cmakeArgs = @('-S', (Join-Path $PSScriptRoot 'cpp'), '-B', (Join-Path $PSScriptRoot 'build/cpp'))
 if ($DependencyPrefix) { $cmakeArgs += "-DCMAKE_PREFIX_PATH=$DependencyPrefix" }
 & cmake @cmakeArgs
 if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
 & cmake --build (Join-Path $PSScriptRoot 'build/cpp') --config Release
 if ($LASTEXITCODE -ne 0) { throw 'C++ build failed' }
 $cppRelease = Join-Path $PSScriptRoot 'build/cpp/Release'
 if (-not (Test-Path -LiteralPath (Join-Path $cppRelease 'EmbedAiChatCpp.exe'))) { $cppRelease = Join-Path $PSScriptRoot 'build/cpp' }
 New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'bin/cpp') | Out-Null
 foreach ($artifact in @('EmbedAiChatCpp.exe','embed_ai_chat.lib','EmbedAiChatJsonTests.exe')) {
  $source = Join-Path $cppRelease $artifact
  if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $PSScriptRoot "bin/cpp/$artifact") -Force }
 }
}
if ('java' -in $Languages) { & (Join-Path $PSScriptRoot 'java/build.ps1') }
if ('go' -in $Languages) {
 New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'bin/go') | Out-Null
 Push-Location (Join-Path $PSScriptRoot 'go')
 try {
  & go build -trimpath -o (Join-Path $PSScriptRoot 'bin/go/EmbedAiChatGo.exe') ./cmd/embedai
  if ($LASTEXITCODE -ne 0) { throw 'Go build failed' }
 } finally { Pop-Location }
}
if ('rust' -in $Languages) {
 & cargo build --manifest-path (Join-Path $PSScriptRoot 'rust/Cargo.toml') --target-dir (Join-Path $PSScriptRoot 'rust/target') --release --locked
 if ($LASTEXITCODE -ne 0) { throw 'Rust build failed' }
 New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'bin/rust') | Out-Null
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'rust/target/release/EmbedAiChatRust.exe') -Destination (Join-Path $PSScriptRoot 'bin/rust') -Force
 # 同时导出可供宿主链接的 Rust 库。
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'rust/target/release/libembed_ai_chat.rlib') -Destination (Join-Path $PSScriptRoot 'bin/rust') -Force
}
if ('csharp' -in $Languages) {
 & dotnet publish (Join-Path $PSScriptRoot 'csharp/Console/Console.csproj') -c Release --no-self-contained -o (Join-Path $PSScriptRoot 'bin/csharp') --nologo
 if ($LASTEXITCODE -ne 0) { throw 'C# build failed' }
}
