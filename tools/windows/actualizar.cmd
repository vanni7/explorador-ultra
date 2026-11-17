@echo off
rem Cierra Explorador Ultra, compila la version de la carpeta compartida, pasa las
rem pruebas y, si todo sale bien, la vuelve a abrir. Uso en la VM: Win+R y
rem    cmd /k \\vboxsvr\proyecto\tools\windows\actualizar.cmd
title Actualizar Explorador Ultra
echo Cerrando Explorador Ultra para compilar la version nueva...
taskkill /im explorador-ultra.exe >nul 2>&1
timeout /t 4 /nobreak >nul
call \\vboxsvr\proyecto\tools\windows\build.cmd
if errorlevel 1 (echo. & echo *** LA COMPILACION O LAS PRUEBAS FALLARON *** & exit /b 1)
start "" \\vboxsvr\proyecto\tools\windows\abrir-explorador.cmd
echo. & echo *** LISTO: version nueva compilada, probada y abierta ***
