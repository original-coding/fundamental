@echo off
pushd %~dp0

set "FILE=%~1"
set "SERVER=%~2"
set "REMOTE=%~3"

if "%FILE%"=="" (
    echo usage: %~nx0 ^<file^> [host:port] [remote_name]
    exit /b 1
)

if "%SERVER%"=="" set "SERVER=127.0.0.1:9000"
if "%REMOTE%"=="" set "REMOTE=%~nx1"

if not exist "%FILE%" (
    echo file not found: %FILE%
    exit /b 1
)

curl.exe -fS -T "%FILE%" "http://%SERVER%/upload?path=%REMOTE%"
