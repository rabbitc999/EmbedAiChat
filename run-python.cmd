@echo off
chcp 65001 >nul
cd /d "%~dp0"
python -X utf8 python\console.py --config examples\config.deepseek.json
pause
