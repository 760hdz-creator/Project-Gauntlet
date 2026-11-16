@echo off
rem Compile lifted C (syntax/codegen check). Usage: tools\build_lifted.bat [file.c]
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d "%~dp0..\out\lifted"
if not "%1"=="" (
  clang-cl /nologo /c /O2 /I..\..\recomp\include -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses-equality %1
  exit /b %errorlevel%
)
for %%f in (f_*.c functable.c) do (
  clang-cl /nologo /c /O2 /I..\..\recomp\include -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses-equality %%f 2>> build_errors.txt || echo FAIL %%f
)
