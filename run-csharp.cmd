@echo off
chcp 65001 >nul
cd /d "%~dp0"
dotnet bin\csharp\EmbedAiChatCSharp.dll --config examples\config.deepseek.json
pause
