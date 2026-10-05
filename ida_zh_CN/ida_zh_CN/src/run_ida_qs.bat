@echo off
rem Launch IDA GUI without -A so the Quick Start dialog shows (no file opened)
cd /d "D:\Program Files\IDA Professional 9.5"
start "" ida.exe -L"C:\Users\Administrator\AppData\Roaming\Hex-Rays\IDA Pro\plugins\ida_zh_CN\idalog.txt"
exit /b 0
