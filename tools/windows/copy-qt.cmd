@echo off
rem Copia Qt desde la carpeta compartida "qtwin" a C:\Qt
robocopy \\vboxsvr\qtwin\6.12.0 C:\Qt\6.12.0 /E /NFL /NDL /NJH /NP /MT:8
if exist C:\Qt\6.12.0\msvc2022_64\bin\qmake.exe echo QT_COPIADO
