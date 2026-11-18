@echo off
rem Build out\gauntlet.exe from runtime\src + lifted objects. Run tools\build_lifted.bat first.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d "%~dp0..\out"
if not exist rt mkdir rt
for %%f in (..\runtime\src\*.c) do (
  clang-cl /nologo /c /O2 /Z7 /I..\recomp\include /I..\runtime\src -D_CRT_SECURE_NO_WARNINGS %%f /Fort\%%~nf.obj || exit /b 1
)
lld-link /nologo /DEBUG /OUT:gauntlet.exe /MAP:gauntlet.map rt\*.obj lifted\*.obj libcmt.lib kernel32.lib winmm.lib user32.lib gdi32.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib shlwapi.lib winhttp.lib || exit /b 1
