#!/bin/bash
#Release Debug 
cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DENABLE_CLANG_TIDY_CHECK=ON -DCMAKE_BUILD_TYPE:STRING="Debug"  -S . -B ./build-linux-debug-clang 

