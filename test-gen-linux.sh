#!/bin/bash
#Release Debug 
cmake  -DCMAKE_BUILD_TYPE:STRING="RelWithDebInfo" $@  -S . -B ./build-linux 

