@echo off
rem Instala con vcpkg las bibliotecas de C que Explorador Ultra usa en Windows:
rem libarchive (comprimidos y documentos de oficina) con zlib, bzip2, xz, zstd y lz4.
rem Sin OpenSSL: en Windows libarchive cifra y descifra con CNG, del sistema.
rem Uso: instalar-vcpkg.cmd [carpeta de vcpkg, por defecto C:\vcpkg]
rem La version de vcpkg esta fijada (VCPKG_TAG) para que todos compilen lo mismo.
setlocal
set VCPKG_TAG=2026.10.16
set VCPKG_PAQUETES=libarchive[core,bzip2,lz4,lzma,zstd]
set VCPKG_ROOT=%~1
if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=C:\vcpkg
if not exist "%VCPKG_ROOT%\.git" (
  git clone --quiet --depth 1 --branch %VCPKG_TAG% https://github.com/microsoft/vcpkg "%VCPKG_ROOT%" || exit /b 1
)
set ACTUAL=
for /f %%t in ('git -C "%VCPKG_ROOT%" describe --tags --exact-match 2^>nul') do set ACTUAL=%%t
if not "%ACTUAL%"=="%VCPKG_TAG%" (
  echo vcpkg esta en "%ACTUAL%"; se cambia a %VCPKG_TAG%
  git -C "%VCPKG_ROOT%" fetch --quiet --depth 1 origin tag %VCPKG_TAG% || exit /b 1
  git -C "%VCPKG_ROOT%" checkout --quiet %VCPKG_TAG% || exit /b 1
  if exist "%VCPKG_ROOT%\vcpkg.exe" del "%VCPKG_ROOT%\vcpkg.exe"
)
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics || exit /b 1
)
rem El triplete propio se copia a una ruta local: vcpkg no siempre acepta rutas \\servidor.
robocopy "%~dp0vcpkg-triplets" "%VCPKG_ROOT%\eu-triplets" /NFL /NDL /NJH /NJS /NP >nul
"%VCPKG_ROOT%\vcpkg.exe" install %VCPKG_PAQUETES%:x64-windows-release --overlay-triplets="%VCPKG_ROOT%\eu-triplets" --disable-metrics || exit /b 1
echo VCPKG_LISTO
