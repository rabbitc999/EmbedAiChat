@echo off
chcp 65001 >nul
cd /d "%~dp0"
bin\go\EmbedAiChatGo.exe --config examples\config.deepseek.json
pause
