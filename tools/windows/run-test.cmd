@echo off
rem Ejecuta una prueba suelta en la VM mostrando toda su salida.
set PATH=C:\Qt\6.12.0\msvc2022_64\bin;%PATH%
set QT_QPA_PLATFORM=offscreen
cd /d C:\eu\src\build\release\bin
%1.exe
echo codigo=%ERRORLEVEL%
