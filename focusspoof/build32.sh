#!/bin/bash
cd "I:/NBOX2024-9-6/YuanSunshine/Sunshine-2026.906.222525/focusspoof"
export PATH=/d/msys64/mingw32/bin:$PATH
g++ -shared -O2 -s focusspoof.cpp -o focusspoof32.dll -static -lws2_32 -lole32 -loleaut32 2>&1
echo "EXIT=$?"
ls -la focusspoof32.dll 2>&1