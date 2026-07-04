@echo off
REM Build the web version on Windows. Requires the Emscripten SDK activated:
REM     C:\path\to\emsdk\emsdk_env.bat
REM     build.bat
REM Output lands in webgame\dist\. Serve over HTTP (see README); file:// won't work.
setlocal
cd /d "%~dp0"

where emcmake >nul 2>nul
if errorlevel 1 (
  echo error: emcmake not found. Activate the Emscripten SDK first:
  echo        C:\path\to\emsdk\emsdk_env.bat
  exit /b 1
)

set BUILD_DIR=build-em
call emcmake cmake -B %BUILD_DIR% -DCMAKE_BUILD_TYPE=Release || exit /b 1
call emmake cmake --build %BUILD_DIR% -j || exit /b 1

echo.
echo Build complete. Artifacts in dist\:
dir /b dist
echo.
echo Serve locally with:  python -m http.server 8000   (then open http://localhost:8000/)
endlocal
