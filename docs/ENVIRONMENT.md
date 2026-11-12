# Entorno de desarrollo

Registro de lo instalado, para poder reproducirlo. Equipo de desarrollo: Ubuntu 26.04.1 LTS, kernel 7.0.0-43, sesión GNOME en Wayland.

## 2026-11-11 — Fase 0

### Ya estaban instalados
| Herramienta | Versión |
|---|---|
| GCC / G++ | 15.2.0 |
| Git | 2.53.0 |
| GDB | 17.1 |
| perf | 7.0.14 |
| Python | 3.14.4 |
| VirtualBox + Extension Pack | 7.2.22 r175245 |

### Instalado con apt
```bash
sudo apt-get install -y build-essential cmake ninja-build ccache clang clang-tidy clang-format lld \
  valgrind gh pipx pkg-config hyperfine \
  libgl-dev libegl-dev libxkbcommon-dev libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 \
  libxcb-keysyms1 libxcb-render-util0 libxcb-xinerama0 libxcb-xkb1 libfontconfig1-dev libfreetype-dev \
  libwayland-dev libdbus-1-dev \
  libarchive-dev libexiv2-dev libtag-dev libraw-dev libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libsqlite3-dev libxxhash-dev zlib1g-dev libssl-dev libgtest-dev \
  librsvg2-bin icoutils icnsutils imagemagick
```
| Herramienta | Versión |
|---|---|
| CMake | 4.2.3 |
| Clang / clang-tidy / clang-format | 21.1.6 |
| GitHub CLI (`gh`) | 2.46.0 |
| libarchive / Exiv2 / TagLib / LibRaw | 3.8.5 / 0.28.8 / 2.2.1 / 0.21.5 |
| FFmpeg (dev) / SQLite / xxHash | 8.0.1 / 3.46.1 / 0.8.3 |

### Qt
```bash
pipx install aqtinstall
aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O ~/Qt -m qtimageformats qtmultimedia qtpdf qtshadertools
export EU_QT_DIR=$HOME/Qt/6.12.0/gcc_64
```
Qt 6.12.0 queda en `~/Qt/6.12.0/gcc_64` (no toca el Qt del sistema).

### Máquina virtual de Windows
- ISO oficial de evaluación: Windows 11 Enterprise 26H2 (compilación 26300.9457), español, 90 días. Descargada del Centro de evaluación de Microsoft a `~/VMs/iso/Win11_Enterprise_Eval_es.iso` (8,2 GB).
- VM `EU-Win11-Build` en `~/VMs/EU-Win11-Build`: 4 CPU, 8 GB de RAM, EFI con arranque seguro, TPM 2.0, disco dinámico de 100 GB, red NAT.
```bash
VBoxManage createvm --name EU-Win11-Build --ostype Windows11_64 --basefolder ~/VMs --register
VBoxManage modifyvm EU-Win11-Build --memory 8192 --cpus 4 --vram 128 --firmware efi --tpm-type 2.0 \
  --ioapic on --graphicscontroller vboxsvga --nic1 nat --audio-driver none --clipboard-mode bidirectional \
  --usb-xhci on --mouse usbtablet
VBoxManage modifynvram EU-Win11-Build inituefivarstore
VBoxManage modifynvram EU-Win11-Build enrollmssignatures
VBoxManage modifynvram EU-Win11-Build enrollorclpk
VBoxManage createmedium disk --filename ~/VMs/EU-Win11-Build/EU-Win11-Build.vdi --size 102400
VBoxManage storagectl EU-Win11-Build --name SATA --add sata --controller IntelAhci --portcount 4
VBoxManage storageattach EU-Win11-Build --storagectl SATA --port 0 --device 0 --type hdd \
  --medium ~/VMs/EU-Win11-Build/EU-Win11-Build.vdi --nonrotational on --discard off   # con "--discard on" la instalación se cuelga (probado)
VBoxManage unattended install EU-Win11-Build --iso ~/VMs/iso/Win11_Enterprise_Eval_es.iso \
  --user eu --password '<contraseña local de la VM>' --locale es_ES --country MX \
  --install-additions --additions-iso /usr/share/virtualbox/VBoxGuestAdditions.iso --image-index 1 --start-vm headless
```
- Usuario local de la VM: `eu`. La contraseña es solo de esa VM de pruebas y no se guarda en el repositorio.
- Las demás VM del equipo no forman parte del proyecto y no se modifican.

### Herramientas dentro de la VM (instaladas el 2026-11-11)
| Herramienta | Versión / origen |
|---|---|
| Visual Studio Build Tools 2022 (MSVC, carga "Desarrollo de escritorio con C++") | `https://aka.ms/vs/17/release/vs_BuildTools.exe` |
| CMake | 4.4.4 (MSI oficial) |
| Git para Windows | 2.56.0.4 |
| Python | 3.13.17 |
| Qt | 6.12.0 `msvc2022_64` en `C:\Qt` (sin Qt PDF por ahora) |

Cómo se hizo (guiones en `tools/windows/`):
1. **UAC desactivado en la VM** (`EnableLUA=0`), porque las órdenes enviadas con `VBoxManage guestcontrol` no pueden aceptar el aviso de administrador. Se hizo una vez desde un PowerShell elevado abierto con el teclado virtual. Consecuencia: en esa VM ya no funcionan las aplicaciones de la tienda, incluido `winget`; por eso los guiones descargan los instaladores con `curl`.
2. `provision-launch.cmd` ejecuta `provision.cmd` como tarea programada (cuenta SYSTEM), independiente de la sesión remota, con registro en `C:\eu\provision.log`. `status.cmd` muestra el avance.
3. Qt: `aqtinstall` 3.3.0 **no encuentra Qt 6.12 para Windows** (busca la carpeta `qt6_6120`; el repositorio ahora usa `qt6_6120_msvc2022_64`). Se descarga en Ubuntu con `tools/windows/fetch_qt_windows.py ~/VMs/qt-windows` (comprueba las sumas SHA-1; requiere `sudo apt install 7zip`) y se copia a la VM con `copy-qt.cmd` por la carpeta compartida `qtwin`.
4. Compilar y probar: `build.cmd` copia el código de la carpeta compartida `proyecto` (solo lectura) a `C:\eu\src`, compila con MSVC + Ninja y ejecuta las pruebas.

```bash
# Desde Ubuntu, con la VM encendida:
VBoxManage guestcontrol EU-Win11-Build run --username eu --password '<contraseña>' \
  --exe 'C:\Windows\System32\cmd.exe' -- cmd /c '\\vboxsvr\proyecto\tools\windows\build.cmd'
```
Instantáneas: `windows-recien-instalado` y `herramientas-instaladas`. El teclado de la VM es de distribución estadounidense.

### Pendiente en el entorno
- Autenticación de `gh` y creación del repositorio en GitHub.
- Qt PDF para Windows (repositorio "extensions"), necesario en la Fase 3.
