; Inno Setup script for Wireless Battery (optional alternative to the native Win32 installer).
#ifndef AppVersion
  #define AppVersion "1.0.8"
#endif

[Setup]
AppId={{8F6D3C4E-7B51-4F2A-9D1C-3E8A5B2C6F90}
AppName=Kablosuz Cihaz Pil İzleyici (Wireless Battery)
AppVersion={#AppVersion}
AppVerName=Kablosuz Cihaz Pil İzleyici {#AppVersion}
VersionInfoVersion={#AppVersion}.0
DefaultDirName={autopf}\WirelessBattery
DefaultGroupName=Kablosuz Pil İzleyici
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=WirelessBattery-Setup-{#AppVersion}
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\WirelessBattery.exe
UninstallDisplayName=Kablosuz Cihaz Pil İzleyici
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=force

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Windows başlangıcında otomatik çalıştır (Run at Windows startup)"; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "..\WirelessBattery.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\headsetcontrol\headsetcontrol.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Kablosuz Pil İzleyici"; Filename: "{app}\WirelessBattery.exe"; Comment: "Kablosuz cihaz pil seviyelerini görev çubuğunda gösterir"
Name: "{autodesktop}\Kablosuz Pil İzleyici"; Filename: "{app}\WirelessBattery.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "WirelessBattery"; ValueData: """{app}\WirelessBattery.exe"""; Flags: uninsdeletevalue; Tasks: startup
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "WirelessBattery"; Flags: dontcreatekey deletevalue; Tasks: not startup
Root: HKCU; Subkey: "Software\WirelessBattery"; Flags: uninsdeletekey

[Run]
Filename: "{app}\WirelessBattery.exe"; Description: "Kablosuz Pil İzleyici'yi başlat"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/F /IM WirelessBattery.exe"; Flags: runhidden; RunOnceId: "StopWirelessBattery"
