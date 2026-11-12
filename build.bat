@echo off
rem Build bad_apple_notepad.exe with MSVC if available, otherwise MinGW gcc.
setlocal

where cl >nul 2>nul
if not errorlevel 1 (
    echo [msvc] cl /O2 /W3 bad_apple_notepad.c /link winmm.lib gdi32.lib
    cl /nologo /O2 /W3 bad_apple_notepad.c /link winmm.lib gdi32.lib
    goto :done
)

where gcc >nul 2>nul
if not errorlevel 1 (
    echo [mingw] gcc -O2 -Wall -Wextra -std=c99 bad_apple_notepad.c -o bad_apple_notepad.exe -lwinmm -lgdi32
    gcc -O2 -Wall -Wextra -std=c99 bad_apple_notepad.c -o bad_apple_notepad.exe -lwinmm -lgdi32
    goto :done
)

echo error: neither cl.exe nor gcc.exe found on PATH.
exit /b 1

:done
if errorlevel 1 exit /b 1
echo built bad_apple_notepad.exe
