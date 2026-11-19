@echo off
rem Cierra Explorador Ultra, compila la version de la carpeta compartida, pasa las
rem pruebas y la vuelve a abrir. Si algo falla, abre la version que haya quedado
rem (la anterior si no llego a enlazarse) para no dejar al usuario sin ella.
rem Uso en la VM: Win+R y
rem    cmd /k \\vboxsvr\proyecto\tools\windows\actualizar.cmd
title Actualizar Explorador Ultra
echo Cerrando Explorador Ultra para compilar la version nueva...
taskkill /im explorador-ultra.exe >nul 2>&1
timeout /t 4 /nobreak >nul
call \\vboxsvr\proyecto\tools\windows\build.cmd
if errorlevel 1 goto fallo
start "" \\vboxsvr\proyecto\tools\windows\abrir-explorador.cmd
echo. & echo *** LISTO: version nueva compilada, probada y abierta ***
exit /b 0
:fallo
echo.
echo *** LA COMPILACION O LAS PRUEBAS FALLARON (detalle arriba) ***
echo Se abre la version que haya quedado para que puedas seguir usandola.
start "" \\vboxsvr\proyecto\tools\windows\abrir-explorador.cmd
exit /b 1
