@echo off
rem Launch a test IDA instance on a harmless small target, log to idalog.txt
rem Args: %1 = target file (optional)
cd /d "D:\Program Files\IDA Professional 9.5"
if "%~1"=="" (
  start "" ida.exe -A -L"C:\Users\Administrator\AppData\Roaming\Hex-Rays\IDA Pro\plugins\ida_zh_CN\idalog.txt"
) else (
  start "" ida.exe -A -L"C:\Users\Administrator\AppData\Roaming\Hex-Rays\IDA Pro\plugins\ida_zh_CN\idalog.txt" "%~1"
)
exit /b 0
