$ErrorActionPreference = 'Stop'
# 全部验收使用本地模型与 MCP fixture，不读取用户密钥，不调用收费模型。
& python -X utf8 -m unittest discover -s (Join-Path $PSScriptRoot 'python/tests') -v
if ($LASTEXITCODE -ne 0) { throw 'Python boundary tests failed' }
& ctest --test-dir (Join-Path $PSScriptRoot 'build/c') -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'C core/HAL tests failed' }
& python -X utf8 (Join-Path $PSScriptRoot 'tests/verify_c.py')
if ($LASTEXITCODE -ne 0) { throw 'C HTTP acceptance failed' }
& ctest --test-dir (Join-Path $PSScriptRoot 'build/cpp') -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Native JSON tests failed' }
& python -X utf8 (Join-Path $PSScriptRoot 'tests/verify.py') `
 --cpp (Join-Path $PSScriptRoot 'bin/cpp/EmbedAiChatCpp.exe') `
 --go (Join-Path $PSScriptRoot 'bin/go/EmbedAiChatGo.exe') `
 --rust (Join-Path $PSScriptRoot 'bin/rust/EmbedAiChatRust.exe') `
 --csharp (Join-Path $PSScriptRoot 'bin/csharp/EmbedAiChatCSharp.dll') `
 --python
if ($LASTEXITCODE -ne 0) { throw 'Cross-language acceptance failed' }

& python -X utf8 (Join-Path $PSScriptRoot 'tests/verify_tokens.py')
if ($LASTEXITCODE -ne 0) { throw 'Token configuration acceptance failed' }

& python -X utf8 (Join-Path $PSScriptRoot 'tests/verify_mcp_feedback.py')
if ($LASTEXITCODE -ne 0) { throw 'MCP feedback acceptance failed' }

& python -X utf8 (Join-Path $PSScriptRoot 'tests/verify_c_mcu.py')
if ($LASTEXITCODE -ne 0) { throw 'C MCU profile acceptance failed' }
