@echo off
rem Link lifted objects + runtime\oracle\oracle.c into out\oracle.dll.
rem Run tools\build_lifted.bat first.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d "%~dp0..\out"
clang-cl /nologo /c /O2 /I..\recomp\include ..\runtime\oracle\oracle.c /Fooracle.obj || exit /b 1
lld-link /nologo /DLL /MAP:oracle.map /OUT:oracle.dll oracle.obj lifted\*.obj libcmt.lib || exit /b 1
