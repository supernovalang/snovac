; snovac.iss — Inno Setup Script for Snovalang Compiler (snovac)
; Builds a native Windows setup installer (snovac-setup.exe)

#define MyAppName "Snovalang Compiler"
#define MyAppShortName "snovac"
#define MyAppVersion "0.0.1-p1"
#define MyAppPublisher "Snovalang Project"
#define MyAppURL "https://github.com/supernovalang/snovac"
#define MyAppExeName "snovac.exe"

[Setup]
AppId={{D37E7498-84BE-4B69-9524-2C7E17C82C6A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\Snova
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=..\..\build\installer
OutputBaseFilename=snovac-windows-x86_64-setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "envPath"; Description: "Add snovac to Environment PATH"; GroupDescription: "System Integration:"; Flags: checkedonce

[Files]
Source: "..\..\build\snovac.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\..\build\libsnovart.a"; DestDir: "{app}\lib"; Flags: ignoreversion
Source: "..\..\*.h"; DestDir: "{app}\include"; Flags: ignoreversion
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: string; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; Tasks: envPath; Check: NeedsAddPath(ExpandConstant('{app}\bin'))

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath)
  then begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + UpperCase(Param) + ';', ';' + UpperCase(OrigPath) + ';') = 0;
end;
