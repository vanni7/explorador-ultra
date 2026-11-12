@echo off
rem Lanza provision.cmd desligado de la sesion remota y guarda la salida en C:\eu\provision.log
mkdir C:\eu 2>nul
copy /y \\vboxsvr\proyecto\tools\windows\provision.cmd C:\eu\provision.cmd >nul
schtasks /create /f /tn eu-provision /sc once /st 00:00 /rl highest /ru SYSTEM /tr "cmd /c C:\eu\provision.cmd > C:\eu\provision.log 2>&1"
schtasks /run /tn eu-provision
