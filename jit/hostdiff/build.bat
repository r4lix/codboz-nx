@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
rem /fp:precise keeps the compiler from contracting the interpreter's separate
rem multiply and accumulate into an FMA -- VFP's VMLA/VMLS are not fused.
cl /nologo /W3 /O2 /fp:precise /D_CRT_SECURE_NO_WARNINGS /I.. /I..\..\loader ^
   host_main.c ..\interp.c ..\guest.c ..\s3e_files.c ..\s3e_config.c ^
   ..\..\loader\s3e_loader.c ^
   /Fe:hostrun.exe
