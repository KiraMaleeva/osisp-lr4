@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /O2 /EHsc /std:c++17 /utf-8 src\lab4_queue.cpp /Fe:lab4_queue.exe winmm.lib
