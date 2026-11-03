@echo off
chcp 65001 >nul
cd /d "%~dp0"
bin\c\EmbedAiChatC.exe --config examples\config.deepseek.json
pause
