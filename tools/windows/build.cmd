@echo off
rem Compila y prueba Explorador Ultra dentro de la VM de Windows.
rem Copia el código de la carpeta compartida (solo lectura) a C:\eu\src y compila ahí.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set PATH=C:\Program Files\CMake\bin;C:\Qt\6.12.0\msvc2022_64\bin;%PATH%
set EU_QT_DIR=C:\Qt\6.12.0\msvc2022_64

robocopy \\vboxsvr\proyecto C:\eu\src /MIR /XD build .git /NFL /NDL /NJH /NJS
cd /d C:\eu\src || exit /b 1
cmake --preset release || exit /b 1
cmake --build --preset release || exit /b 1
ctest --preset release || exit /b 1
set QT_QPA_PLATFORM=offscreen
build\release\bin\explorador-ultra.exe --smoke-test
echo smoke-test=%ERRORLEVEL%
