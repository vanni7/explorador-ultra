@echo off
echo --- procesos
tasklist | findstr /i "vs_ setup.exe msiexec python git.exe curl cmd.exe"
echo --- descargas
dir /b C:\eu\descargas
echo --- instalado
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" echo MSVC si
if exist "C:\Program Files\CMake\bin\cmake.exe" echo CMake si
if exist "C:\Program Files\Git\cmd\git.exe" echo Git si
if exist "C:\Program Files\Python313\python.exe" echo Python si
if exist "C:\Qt\6.12.0\msvc2022_64\bin\qmake.exe" echo Qt si
echo --- log
if exist C:\eu\provision.log powershell -NoProfile -Command "Get-Content C:\eu\provision.log -Tail 12"
net statistics workstation | findstr /i "desde since"
