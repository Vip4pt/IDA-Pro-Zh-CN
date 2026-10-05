@echo off
rem 构建 zh_hook.dll (VS 2022 x64)
setlocal
call "D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo VCVARS FAILED
  exit /b 1
)
cd /d "%~dp0"
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /LD /Fe:..\zh_hook.dll zh_hook.c MinHook\src\buffer.c MinHook\src\hook.c MinHook\src\trampoline.c MinHook\src\hde\hde64.c /I MinHook\include /I MinHook\src /link kernel32.lib user32.lib
if errorlevel 1 (
  echo CL FAILED
  exit /b 1
)
echo BUILD OK
endlocal
