; YTTranscript — Inno Setup 6 script
;
; Produces a small (~600 KB) per-user installer. It installs only the launcher
; (YTTranscript.exe); the app downloads its helpers and models (yt-dlp, FFmpeg,
; whisper.cpp, llama.cpp, the Whisper + Qwen models) on first run.
;
; Build:  "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\YTTranscript.iss
; Output: installer\Output\YTTranscript-Setup-1.0.0.exe

#define AppName     "YTTranscript"
#define AppVersion  "1.1.0"
#define AppPublisher "swaub"
#define AppURL      "https://github.com/swaub/YTTranscript"
#define AppExe      "YTTranscript.exe"

[Setup]
AppId={{B7E4B2B0-9C3A-4D6E-8F1A-2C5D7E9F1A3B}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=YTTranscript-Setup-{#AppVersion}
SetupIconFile=..\res\icon.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\dist\{#AppExe}";         DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";                DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";                        Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}";  Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";                  Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
{ The app downloads its components into its own install folder, so that folder
  must be writable by the current (non-admin) user. Validate the chosen path
  before installing and steer the user away from places like Program Files. }
function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir, TestFile: string;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    Dir := WizardDirValue;
    if not DirExists(Dir) then
    begin
      if not ForceDirectories(Dir) then
      begin
        MsgBox('The folder ' + Dir + ' could not be created.' + #13#10
          + 'Please choose a writable location (for example C:\YTTranscript)'
          + ' and avoid Program Files.', mbError, MB_OK);
        Result := False;
        Exit;
      end;
    end;
    TestFile := AddBackslash(Dir) + 'ytt_write_test.tmp';
    if SaveStringToFile(TestFile, 'test', False) then
      DeleteFile(TestFile)
    else
    begin
      MsgBox('YTTranscript needs to download its components into its install'
        + ' folder, but ' + Dir + ' is not writable.' + #13#10
        + 'Please choose a writable location (for example C:\YTTranscript)'
        + ' and avoid Program Files.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

{ On uninstall, offer to remove the (large) downloaded components and user data.
  Default to keeping them so a reinstall does not re-download ~2 GB. }
procedure CurUninstallStepChanged(CurStep: TUninstallStep);
var
  App: string;
begin
  if CurStep = usUninstall then
  begin
    App := ExpandConstant('{app}');
    if MsgBox('Also delete the downloaded components and data in ' + App + '?'
        + #13#10 + '(bin, models, output, temp, and settings)' + #13#10#13#10
        + 'Choose No to keep them - useful if you plan to reinstall, since it'
        + ' avoids re-downloading about 2 GB.',
        mbConfirmation, MB_YESNO) = IDYES then
    begin
      DelTree(App + '\bin', True, True, True);
      DelTree(App + '\models', True, True, True);
      DelTree(App + '\output', True, True, True);
      DelTree(App + '\temp', True, True, True);
      DeleteFile(App + '\installed.cfg');
      DeleteFile(App + '\settings.cfg');
      DeleteFile(App + '\summ_perf.cfg');
    end;
  end;
end;
