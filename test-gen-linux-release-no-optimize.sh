#!/bin/bash
#Release Debug 
cmake -DF_ENABLE_COMPILE_OPTIMIZE=OFF -DCMAKE_BUILD_TYPE:STRING="Release"  -S . -B ./build-linux-r-no-optimize 

