@echo off
rem Build dist\ProjectGauntlet-Setup-<VERSION>.exe: stages dist\payload.zip (tools\package.py), then compiles the
rem installer with the payload, art and icon embedded. New tracked files must be `git add`ed first (package.py
rem stages from git ls-files).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d "%~dp0"
set /p GT_VER=<..\VERSION
python -I ..\tools\package.py || exit /b 1
rc /nologo /fo setup.res setup.rc || exit /b 1
clang-cl /nologo /O2 /DGT_SETUP_VERSION=\"%GT_VER%\" /std:c++20 /EHsc /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS setup.cpp setup.res /Fe:..\dist\ProjectGauntlet-Setup-%GT_VER%.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib windowscodecs.lib msimg32.lib advapi32.lib uxtheme.lib || exit /b 1
del setup.obj 2>nul
echo Built dist\ProjectGauntlet-Setup-%GT_VER%.exe
