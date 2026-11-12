@echo off
rem Prepara la VM de Windows para compilar Explorador Ultra.
rem Se ejecuta una sola vez dentro de Windows, con privilegios de administrador.
rem No usa winget: con UAC desactivado Windows no ejecuta aplicaciones de la tienda.
setlocal
set DL=C:\eu\descargas
mkdir %DL% 2>nul
cd /d %DL%

echo === Visual Studio Build Tools 2022 (MSVC; incluye CMake y Ninja) ===
curl -sSL -o vs_BuildTools.exe https://aka.ms/vs/17/release/vs_BuildTools.exe
vs_BuildTools.exe --quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended
echo codigo=%ERRORLEVEL%

echo === CMake ===
curl -sSL -o cmake.msi https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.msi
msiexec /i cmake.msi /qn /norestart
echo codigo=%ERRORLEVEL%

echo === Git ===
curl -sSL -o git.exe https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.4/Git-2.56.0.4-64-bit.exe
git.exe /VERYSILENT /NORESTART /SUPPRESSMSGBOXES
echo codigo=%ERRORLEVEL%

echo === Python ===
curl -sSL -o python.exe https://www.python.org/ftp/python/3.13.17/python-3.13.17-amd64.exe
python.exe /quiet InstallAllUsers=1 PrependPath=1 Include_test=0
echo codigo=%ERRORLEVEL%

echo === Qt 6.12.0 con aqtinstall ===
"C:\Program Files\Python313\python.exe" -m pip install --quiet aqtinstall
"C:\Program Files\Python313\python.exe" -m aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 -O C:\Qt -m qtimageformats qtmultimedia qtpdf qtshadertools
echo codigo=%ERRORLEVEL%

echo === FIN ===
