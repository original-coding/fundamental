#!/bin/bash
#Release Debug
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
export CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-${SCRIPT_DIR}/sysroot}" 
cmake  -DCMAKE_BUILD_TYPE:STRING="Release" -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH} -DCMAKE_INSTALL_PREFIX=${CMAKE_PREFIX_PATH} -DUSE_PRECOMPILE_FUNDAMENTAL_DEPS=ON  -S . -B ./build-linux-precompile 

