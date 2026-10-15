#!/bin/bash
export PATH="/d/msys64/mingw32/bin:$PATH"
D="I:/NBOX2024-9-6/YuanSunshine/Sunshine-2026.906.222525/focusspoof"
cd "$D"
echo "compiling 32-bit..."
g++ -shared -O2 -s focusspoof.cpp -o focusspoof32_new.dll -static -lws2_32 -lole32 -loleaut32 2>build_err32.txt
echo "EXIT32=$?"
ls -la focusspoof32_new.dll 2>&1
cat build_err32.txt 2>&1