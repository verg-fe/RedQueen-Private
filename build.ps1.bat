@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 (
    echo [ERROR] vcvars32 failed
    exit /b 1
)
set SRC=G:\LocalCodex\CodexSessions\2026-11-11_04-38-46_a628a9d6\ra3fog\src
set OUT=G:\LocalCodex\CodexSessions\2026-11-11_04-38-46_a628a9d6\ra3fog\build
if not exist "%OUT%" mkdir "%OUT%"
echo Compiling ra3fog d3d9 proxy...
cl /LD /O2 /EHsc /W3 /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0501 /D_CRT_SECURE_NO_WARNINGS ^
    "%SRC%\d3d9proxy.cpp" "%SRC%\ra3fog_engine.cpp" "%SRC%\ra3hud.cpp" "%SRC%\ra3macro.cpp" ^
    /I "%SRC%" ^
    /link /OUT:"%OUT%\d3d9.dll" /DEF:"%SRC%\d3d9proxy.def" ^
    d3d9.lib user32.lib gdi32.lib kernel32.lib
if errorlevel 1 (
    echo [ERROR] Compilation failed
    exit /b 1
)
echo [OK] Build complete: %OUT%\d3d9.dll
exit /b 0
