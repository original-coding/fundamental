@echo off
pushd %~dp0
:: 如果 VCPKG_ROOT 已定义且不为空，直接使用
if defined VCPKG_ROOT (
    if not "%VCPKG_ROOT%"=="" (
        set "VCPKG_TOOLCHAIN=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
        echo Using env VCPKG_ROOT=%VCPKG_ROOT%
        goto :check_toolchain
    )
)

for /f "delims=" %%i in ('where vcpkg 2^>nul') do (
    set "VCPKG_ROOT=%%~dpi"
    goto :check_toolchain
)
echo not found vcpkg,add vcpakg to PATH。
exit /b 1

:check_toolchain
:: 后续使用 %VCPKG_ROOT%

set VCPKG_TOOLCHAIN="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"


:: 调用 CMake
if defined VCPKG_TOOLCHAIN (
    echo Using vcpkg toolchain: %VCPKG_TOOLCHAIN%
    cmake -DCMAKE_TOOLCHAIN_FILE=%VCPKG_TOOLCHAIN%  -S . -B ./build-win 
) else (
    echo [WARNING] vcpkg.cmake not found. Proceeding without vcpkg.
    cmake -S . -B ./build-win
)

pause
popd

