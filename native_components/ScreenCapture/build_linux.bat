@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
docker build --platform linux/amd64 -f "%SCRIPT_DIR%\Dockerfile.linux" -t screen-capture-builder "%SCRIPT_DIR%"
if errorlevel 1 exit /b 1
docker run --rm --platform linux/amd64 -v "%SCRIPT_DIR%:/src" screen-capture-builder bash -c "tr -d '\r' < /src/build_linux_inner.sh | bash"
if errorlevel 1 exit /b 1
echo Done: %SCRIPT_DIR%\build_linux\ScreenCapture.so
