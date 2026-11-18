; Inno Setup script for the Windows installer (built by CI, see .github/workflows/build.yml).
; iscc /DAppVersion=1.0.0 /DSourceDir=..\dist js8mesh.iss
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif

[Setup]
AppId={{6D0B3B8E-5C1A-4E8B-9C33-2F1E7A4B9D10}
AppName=JS8 Mesh Gateway
AppVersion={#AppVersion}
AppPublisher=WM8Q
AppPublisherURL=https://github.com/avantol/Subspace-Meshtastic-Basic
DefaultDirName={autopf}\JS8 Mesh Gateway
DefaultGroupName=JS8 Mesh Gateway
UninstallDisplayIcon={app}\js8mesh.exe
OutputBaseFilename=js8mesh-{#AppVersion}-windows-setup
OutputDir=..\installer
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startup"; Description: "Start the gateway when Windows starts"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\JS8 Mesh Gateway"; Filename: "{app}\js8mesh.exe"
Name: "{group}\Uninstall JS8 Mesh Gateway"; Filename: "{uninstallexe}"
Name: "{autodesktop}\JS8 Mesh Gateway"; Filename: "{app}\js8mesh.exe"; Tasks: desktopicon
Name: "{userstartup}\JS8 Mesh Gateway"; Filename: "{app}\js8mesh.exe"; Parameters: "--start --minimized"; Tasks: startup

[Run]
Filename: "{app}\js8mesh.exe"; Description: "{cm:LaunchProgram,JS8 Mesh Gateway}"; Flags: nowait postinstall skipifsilent
