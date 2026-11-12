; Inno Setup script for PhoneSweep (https://jrsoftware.org/isinfo.php).
; Built by package.ps1:  ISCC /DAppVersion=1.0.0 /DSourceDir=<staged folder> installer\PhoneSweep.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\PhoneSweep-" + AppVersion + "-win64"
#endif

[Setup]
AppId={{6F0B3C2E-9C1A-4F5B-9B7E-5A8D2E4C1F07}
AppName=PhoneSweep
AppVersion={#AppVersion}
AppVerName=PhoneSweep {#AppVersion}
AppPublisher=Kimscode Studio
AppPublisherURL=https://github.com/KimsFerdy/PhoneSweep
AppSupportURL=https://github.com/KimsFerdy/PhoneSweep/issues
AppUpdatesURL=https://github.com/KimsFerdy/PhoneSweep/releases
AppCopyright=Copyright (c) 2026 Kims Ferdy
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\PhoneSweep
DefaultGroupName=PhoneSweep
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\res\PhoneSweep.ico
UninstallDisplayIcon={app}\PhoneSweep.exe
UninstallDisplayName=PhoneSweep
WizardStyle=modern
; Installs for the current user without admin by default; "all users" is offered too.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
Compression=lzma2/max
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=PhoneSweep-{#AppVersion}-setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\PhoneSweep"; Filename: "{app}\PhoneSweep.exe"
Name: "{autodesktop}\PhoneSweep"; Filename: "{app}\PhoneSweep.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PhoneSweep.exe"; Description: "{cm:LaunchProgram,PhoneSweep}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop the adb server we shipped so its files can be removed.
Filename: "{app}\platform-tools\adb.exe"; Parameters: "kill-server"; Flags: runhidden skipifdoesntexist; RunOnceId: "KillAdb"
