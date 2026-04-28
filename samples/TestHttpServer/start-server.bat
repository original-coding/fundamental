@echo off
pushd %~dp0

set "PORT=%~1"
if "%PORT%"=="" set "PORT=80"

echo [1] script dir: %~dp0
echo [2] current dir: %CD%
echo [3] port: %PORT%
echo [4] launching TestHttpServer.exe...

TestHttpServer.exe "%CD%" %PORT%
pause
