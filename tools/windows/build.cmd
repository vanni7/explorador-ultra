@echo off
rem Compila y prueba Explorador Ultra dentro de la VM de Windows.
rem Copia el código de la carpeta compartida (solo lectura) a C:\eu\src y compila ahí.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set PATH=C:\Program Files\CMake\bin;C:\Qt\6.12.0\msvc2022_64\bin;%PATH%
set EU_QT_DIR=C:\Qt\6.12.0\msvc2022_64
rem Bibliotecas de vcpkg (libarchive), si se instalaron con instalar-vcpkg.cmd.
set EU_CMAKE_ARGS=
if exist C:\vcpkg\scripts\buildsystems\vcpkg.cmake set EU_CMAKE_ARGS=-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-release

robocopy \\vboxsvr\proyecto C:\eu\src /MIR /XD build .git /NFL /NDL /NJH /NJS
cd /d C:\eu\src || exit /b 1
rem El toolchain de vcpkg solo cuenta al configurar desde cero.
if not defined EU_CMAKE_ARGS goto configurar
if not exist build\release\CMakeCache.txt goto configurar
findstr /c:"vcpkg.cmake" build\release\CMakeCache.txt >nul
if errorlevel 1 set EU_CMAKE_ARGS=--fresh %EU_CMAKE_ARGS%
:configurar
cmake --preset release %EU_CMAKE_ARGS% || exit /b 1
cmake --build --preset release || exit /b 1
ctest --preset release || exit /b 1
set QT_QPA_PLATFORM=offscreen
build\release\bin\explorador-ultra.exe --smoke-test
echo smoke-test=%ERRORLEVEL%
