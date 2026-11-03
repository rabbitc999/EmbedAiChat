@echo off
chcp 65001 >nul
cd /d "%~dp0"
java -Dfile.encoding=UTF-8 -cp "java\EmbedAiChatJava.jar;java\lib\gson-2.11.0.jar" io.embedai.Console --config examples\config.deepseek.json
pause
