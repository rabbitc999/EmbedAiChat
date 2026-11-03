$ErrorActionPreference = 'Stop'
# JDK 17+；Gson 从 Maven Central 下载，固定版本及 SHA-256 防止依赖漂移。
$javaRoot = $PSScriptRoot
New-Item -ItemType Directory -Force (Join-Path $javaRoot 'lib'),(Join-Path $javaRoot 'out') | Out-Null
$jarPath = Join-Path $javaRoot 'lib/gson-2.11.0.jar'
if (-not (Test-Path -LiteralPath $jarPath)) {
 Invoke-WebRequest 'https://repo.maven.apache.org/maven2/com/google/code/gson/gson/2.11.0/gson-2.11.0.jar' -OutFile $jarPath
}
# 校验值对应 Gson 2.11.0 官方发布包。
if ((Get-FileHash -LiteralPath $jarPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne '57928d6e5a6edeb2abd3770a8f95ba44dce45f3b23b7a9dc2b309c581552a78b') { throw 'Gson checksum mismatch' }
$sources = @(Get-ChildItem -LiteralPath (Join-Path $javaRoot 'src/main/java/io/embedai') -Filter '*.java' | ForEach-Object FullName)
& javac --release 17 -encoding UTF-8 -cp $jarPath -d (Join-Path $javaRoot 'out') @sources
if ($LASTEXITCODE -ne 0) { throw 'Java compilation failed' }
& jar --create --file (Join-Path $javaRoot 'EmbedAiChatJava.jar') -C (Join-Path $javaRoot 'out') .
if ($LASTEXITCODE -ne 0) { throw 'Java packaging failed' }
