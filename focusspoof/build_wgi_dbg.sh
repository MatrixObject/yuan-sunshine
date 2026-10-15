#!/bin/bash
cd "I:/NBOX2024-9-6/YuanSunshine/Sunshine-2026.906.222525/focusspoof"
export PATH=/c/ProgramData/mingw64/mingw64/bin:$PATH
GXX=g++
echo "compiling 64-bit..."
$GXX -shared -O2 -s focusspoof.cpp -o focusspoof_new.dll -static -lws2_32 -lole32 -loleaut32 2>build_err64.txt
echo "EXIT64=$?"
ls -la focusspoof_new.dll 2>&1
echo "done"