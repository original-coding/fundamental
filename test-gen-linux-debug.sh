#!/bin/bash
#Release Debug
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
export CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-${SCRIPT_DIR}/sysroot}"
cmake -DENABLE_CLANG_TIDY_CHECK=ON -DCMAKE_BUILD_TYPE:STRING="Debug" -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH} -DCMAKE_INSTALL_PREFIX=${CMAKE_PREFIX_PATH}  -S . -B ./build-linux-debug 

