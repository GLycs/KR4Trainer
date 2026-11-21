@echo off
REM ==========================================================================
REM KR4Mod build script (MinGW-w64 GCC, x64)
REM   deps : third_party\imgui-1.90.9 , third_party\minhook
REM   out  : bin\kr4mod.dll , bin\kr4injector.exe
REM   usage: set MINGW=C:\mingw64\bin  (optional, default below)  &&  build.bat
REM ==========================================================================
setlocal

if "%MINGW%"=="" set "MINGW=C:\mingw64\bin"
set "ROOT=%~dp0"
set "IMGUI=%ROOT%third_party\imgui-1.90.9"
set "MINHOOK=%ROOT%third_party\minhook"
set "SRC=%ROOT%src"
set "BIN=%ROOT%bin"
set "OBJ=%ROOT%obj"

if not exist "%IMGUI%\imgui.cpp"    echo [ERROR] missing %IMGUI%   & exit /b 1
if not exist "%MINHOOK%\src\hook.c" echo [ERROR] missing %MINHOOK% & exit /b 1
if not exist "%MINGW%\g++.exe"      echo [ERROR] g++ not found in %MINGW% & exit /b 1

set CXX="%MINGW%\g++.exe"
set CC="%MINGW%\gcc.exe"
set INCS=-I"%SRC%" -I"%IMGUI%" -I"%IMGUI%\backends" -I"%MINHOOK%\include"
set CFLAGS=-O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00

if not exist "%BIN%" mkdir "%BIN%"
if not exist "%OBJ%" mkdir "%OBJ%"

echo [1/5] building imgui core...
%CXX% -c %CFLAGS% -std=c++17 %INCS% "%IMGUI%\imgui.cpp"        -o "%OBJ%\imgui.o"        || goto fail
%CXX% -c %CFLAGS% -std=c++17 %INCS% "%IMGUI%\imgui_draw.cpp"   -o "%OBJ%\imgui_draw.o"   || goto fail
%CXX% -c %CFLAGS% -std=c++17 %INCS% "%IMGUI%\imgui_tables.cpp" -o "%OBJ%\imgui_tables.o" || goto fail
%CXX% -c %CFLAGS% -std=c++17 %INCS% "%IMGUI%\imgui_widgets.cpp" -o "%OBJ%\imgui_widgets.o" || goto fail
%CXX% -c %CFLAGS% -std=c++17 %INCS% "%IMGUI%\imgui_demo.cpp"   -o "%OBJ%\imgui_demo.o"   || goto fail

echo [2/5] building imgui backends (win32 + opengl3)...
%CXX% -c %CFLAGS% -std=c++17 %INCS% "%IMGUI%\backends\imgui_impl_win32.cpp"   -o "%OBJ%\impl_win32.o"   || goto fail
%CXX% -c %CFLAGS% -std=c++17 %INCS% "%IMGUI%\backends\imgui_impl_opengl3.cpp" -o "%OBJ%\impl_opengl3.o" || goto fail

echo [3/5] building MinHook (x64)...
%CC% -c -O2 %INCS% "%MINHOOK%\src\buffer.c"     -o "%OBJ%\mh_buffer.o"     || goto fail
%CC% -c -O2 %INCS% "%MINHOOK%\src\hook.c"       -o "%OBJ%\mh_hook.o"       || goto fail
%CC% -c -O2 %INCS% "%MINHOOK%\src\trampoline.c" -o "%OBJ%\mh_trampoline.o" || goto fail
%CC% -c -O2 %INCS% "%MINHOOK%\src\hde\hde64.c"  -o "%OBJ%\mh_hde64.o"      || goto fail

echo [4/5] building overlay DLL + injector...
%CXX% -c %CFLAGS% -std=c++17 %INCS% "%SRC%\overlay.cpp" -o "%OBJ%\overlay.o" || goto fail

%CXX% -shared -static -static-libgcc -static-libstdc++ -o "%BIN%\kr4mod.dll" ^
  "%OBJ%\overlay.o" "%OBJ%\imgui.o" "%OBJ%\imgui_draw.o" "%OBJ%\imgui_tables.o" ^
  "%OBJ%\imgui_widgets.o" "%OBJ%\imgui_demo.o" "%OBJ%\impl_win32.o" "%OBJ%\impl_opengl3.o" ^
  "%OBJ%\mh_buffer.o" "%OBJ%\mh_hook.o" "%OBJ%\mh_trampoline.o" "%OBJ%\mh_hde64.o" ^
  -lopengl32 -lgdi32 -luser32 -limm32 -ldwmapi -lshcore -lole32 || goto fail

%CXX% -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -o "%BIN%\kr4injector.exe" "%SRC%\injector.cpp" || goto fail

echo [5/5] done.
echo.
echo   DLL      : %BIN%\kr4mod.dll
echo   injector : %BIN%\kr4injector.exe
echo.
exit /b 0

:fail
echo.
echo BUILD FAILED (errorlevel %errorlevel%)
exit /b 1
