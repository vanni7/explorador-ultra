@echo off
rem Abre Explorador Ultra compilado en esta VM (C:\eu\src, con build.cmd).
rem PATH corto y propio: el heredado de la compilacion (vcvars) puede pasar del
rem limite de cmd y salia "No hay suficientes recursos de memoria".
setlocal
set "PATH=C:\Qt\6.12.0\msvc2022_64\bin;%SystemRoot%\System32;%SystemRoot%"
if not exist "C:\eu\src\build\release\bin\explorador-ultra.exe" (
  echo No esta compilado: ejecuta antes build.cmd
  pause
  exit /b 1
)
start "" "C:\eu\src\build\release\bin\explorador-ultra.exe"
exit
