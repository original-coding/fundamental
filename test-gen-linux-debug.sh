#!/bin/bash
#Release Debug 
cmake -DENABLE_CLANG_TIDY_CHECK=ON -DCMAKE_BUILD_TYPE:STRING="Debug"  -S . -B ./build-linux-debug 

