; Instalador de Windows de Explorador Ultra (Inno Setup 6). SPEC §23.
; Parte de la instalación de CMake (cmake --install … --prefix <dist>) y la
; empaqueta. Uso:
;   iscc /DAppVersion=0.1.0 /DSourceDir=C:\ruta\dist explorador-ultra.iss
; El servicio opcional de NTFS (eu-indexsvc.exe) se copia pero no se instala:
; se instala desde la propia aplicación (ventana del índice), con permiso de
; administrador, y al desinstalar la aplicación se quita si quedó instalado.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{D66A1C1E-0343-4ED3-982C-BA392C616F8D}
AppName=Explorador Ultra
AppVersion={#AppVersion}
AppVerName=Explorador Ultra {#AppVersion}
AppPublisher=Explorador Ultra
AppPublisherURL=https://github.com/vanni7/explorador-ultra
AppSupportURL=https://github.com/vanni7/explorador-ultra/issues
DefaultDirName={autopf}\Explorador Ultra
DefaultGroupName=Explorador Ultra
DisableProgramGroupPage=yes
; Para todos los usuarios (Archivos de programa) o, si se elige, solo para el actual.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\assets\icon\explorador-ultra.ico
UninstallDisplayIcon={app}\bin\explorador-ultra.exe
UninstallDisplayName=Explorador Ultra
OutputDir={#OutputDir}
OutputBaseFilename=ExploradorUltra-{#AppVersion}-Windows-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Si la aplicación está abierta, se pide cerrarla antes de reemplazar archivos.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Explorador Ultra"; Filename: "{app}\bin\explorador-ultra.exe"
Name: "{autodesktop}\Explorador Ultra"; Filename: "{app}\bin\explorador-ultra.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\explorador-ultra.exe"; Description: "{cm:LaunchProgram,Explorador Ultra}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Si el usuario instaló el servicio opcional, se quita (si no lo está, no hace nada).
Filename: "{app}\bin\eu-indexsvc.exe"; Parameters: "--desinstalar"; Flags: runhidden waituntilterminated; RunOnceId: "QuitarServicioNTFS"
