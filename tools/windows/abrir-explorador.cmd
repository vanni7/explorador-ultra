@echo off
rem Abre Explorador Ultra compilado en esta VM (C:\eu\src, con build.cmd).
set PATH=C:\Qt\6.12.0\msvc2022_64\bin;%PATH%
if not exist "C:\eu\src\build\release\bin\explorador-ultra.exe" (
  echo No esta compilado: ejecuta antes build.cmd
  pause
  exit /b 1
)
start "" "C:\eu\src\build\release\bin\explorador-ultra.exe"
