#include "src\version.h"

[Setup]
AppId={{46B59A5B-4975-459C-852F-E142DD2D5A25}
AppName=Phahol Remote
AppVersion={#PHAHOL_REMOTE_VERSION}
DefaultDirName={autopf}\Phahol Remote
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\PhaholRemote.exe
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
AppMutex={code:ApplicationMutex}
OutputDir=build
OutputBaseFilename=PhaholRemote-Setup
SetupIconFile=assets\phahol-remote.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "build\PhaholRemote.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{commonprograms}\Phahol Remote"; Filename: "{app}\PhaholRemote.exe"; WorkingDir: "{app}"
Name: "{commonstartup}\Phahol Remote"; Filename: "{app}\PhaholRemote.exe"; Parameters: "--tray"; WorkingDir: "{app}"

[Run]
Filename: "{app}\PhaholRemote.exe"; Description: "Open Phahol Remote"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Tasks]
Name: "remotepower"; Description: "Allow connected Viewers to shut down or restart this Host"; Flags: unchecked

[Registry]
Root: HKLM64; Subkey: "SOFTWARE\Phahol Remote"; ValueType: dword; ValueName: "AllowRemotePower"; ValueData: "1"; Tasks: remotepower; Flags: uninsdeletevalue; Check: ConfigureHost
Root: HKLM64; Subkey: "SOFTWARE\Phahol Remote"; ValueType: dword; ValueName: "AllowRemotePower"; ValueData: "0"; Tasks: not remotepower; Flags: uninsdeletevalue; Check: ConfigureHost

[UninstallDelete]
Type: files; Name: "{app}\trusted-subnets.txt"

[Code]
const
  HostRule = 'Phahol Remote Host TCP';
  ViewerRule = 'Phahol Remote Viewer UDP';
  DomainAndPrivateProfiles = 3;
  Inbound = 1;
  Allow = 1;
  TcpProtocol = 6;
  UdpProtocol = 17;
  DefaultSubnets = '192.168.0.0/16,172.16.0.0/16';
  PreviousDefaultSubnets = '192.168.34.0/24,192.168.0.0/24,172.16.0.0/16';
  UninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{46B59A5B-4975-459C-852F-E142DD2D5A25}_is1';

var
  SubnetPage: TInputQueryWizardPage;
  TrustedSubnets: String;
  AutoAppClosed: Boolean;
  AutoInstallSucceeded: Boolean;

function GetWindowThreadProcessId(Window: HWND; var ProcessId: DWORD): DWORD;
  external 'GetWindowThreadProcessId@user32.dll stdcall';
function SendMessageTimeout(Window: HWND; Msg: UINT; Cookie: WPARAM; Data: LPARAM;
  Flags, Timeout: UINT; var Reply: DWORD_PTR): LRESULT;
  external 'SendMessageTimeoutW@user32.dll stdcall';
function OpenProcess(Access: DWORD; InheritHandle: BOOL; ProcessId: DWORD): HANDLE;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: HANDLE; Timeout: DWORD): DWORD;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: HANDLE): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

function ConfigureHost: Boolean;
begin
  Result := IsUninstaller or (ExpandConstant('{param:PHAHOLUPDATE|}') <> '1');
end;

function ApplicationMutex(Param: String): String;
begin
  if ConfigureHost then
    Result := 'Local\PhaholRemote'
  else
    Result := '';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Installed: String;
  WindowValue, Token: Int64;
  Window: HWND;
  ProcessId: DWORD;
  Process: HANDLE;
  Reply: DWORD_PTR;
begin
  Result := '';
  if ConfigureHost or AutoAppClosed then
    Exit;
  Result := 'The automatic update needs the existing Phahol Remote installation.';
  if not WizardSilent or
     not RegQueryStringValue(HKLM64, UninstallKey, 'InstallLocation', Installed) or
     (CompareText(RemoveBackslashUnlessRoot(Installed), RemoveBackslashUnlessRoot(WizardDirValue)) <> 0) or
     not FileExists(ExpandConstant('{app}\PhaholRemote.exe')) then
    Exit;

  Result := 'Phahol Remote is busy or could not prepare for the update. Try again later.';
  WindowValue := StrToInt64Def(ExpandConstant('{param:PHUPWINDOW|}'), 0);
  Token := StrToInt64Def(ExpandConstant('{param:PHUPTOKEN|}'), 0);
  if (WindowValue <= 0) or (WindowValue > 4294967295) or (Token <= 0) or (Token > 2147483647) then
    Exit;
  Window := HWND(WindowValue);
  if GetWindowThreadProcessId(Window, ProcessId) = 0 then
    Exit;
  Process := OpenProcess($00100000, False, ProcessId);
  if Process = 0 then
    Exit;
  try
    if (SendMessageTimeout(Window, RegisterWindowMessage('PhaholRemote.ApplyVerifiedUpdate.v1'),
      WPARAM(Token), 0, $0003, 5000, Reply) = 0) or (Reply <> 1) then
      Exit;
    if WaitForSingleObject(Process, 15000) <> 0 then
      Exit;
    AutoAppClosed := True;
    Result := '';
  finally
    CloseHandle(Process);
  end;
end;

procedure DeinitializeSetup;
var
  Args: String;
  ErrorCode: Integer;
begin
  if not AutoAppClosed then
    Exit;
  Args := '--tray';
  if not AutoInstallSucceeded then
    Args := '--tray --defer-update';
  if not ExecAsOriginalUser(ExpandConstant('{app}\PhaholRemote.exe'), Args, ExpandConstant('{app}'),
                            SW_SHOWNORMAL, ewNoWait, ErrorCode) then
    Log('Could not reopen Phahol Remote. Error ' + IntToStr(ErrorCode));
end;

procedure InitializeWizard;
var
  SavedSubnets: String;
begin
  SubnetPage := CreateInputQueryPage(wpSelectDir, 'Trusted networks',
    'Allow connections across routed VLANs',
    'Enter trusted IPv4 CIDRs separated by commas. Leave blank for local subnet only.');
  SubnetPage.Add('Additional trusted IPv4 subnets:', False);
  SavedSubnets := GetPreviousData('TrustedSubnets', DefaultSubnets);
  TrustedSubnets := SavedSubnets;
  if SavedSubnets = PreviousDefaultSubnets then
    SavedSubnets := DefaultSubnets;
  SubnetPage.Values[0] := ExpandConstant('{param:TRUSTEDSUBNETS|' + SavedSubnets + '}');
end;

function ValidNumber(const Text: String; Minimum, Maximum: Integer): Boolean;
var
  Value: Integer;
begin
  Value := StrToIntDef(Text, -1);
  Result := (Value >= Minimum) and (Value <= Maximum) and (IntToStr(Value) = Text);
end;

function ValidCidr(const Cidr: String): Boolean;
var
  Address: String;
  Sep, I: Integer;
begin
  Result := False;
  Sep := Pos('/', Cidr);
  if (Sep = 0) or not ValidNumber(Copy(Cidr, Sep + 1, Length(Cidr)), 1, 32) then
    Exit;
  Address := Copy(Cidr, 1, Sep - 1);
  for I := 1 to 3 do
  begin
    Sep := Pos('.', Address);
    if (Sep = 0) or not ValidNumber(Copy(Address, 1, Sep - 1), 0, 255) then
      Exit;
    Delete(Address, 1, Sep);
  end;
  Result := ValidNumber(Address, 0, 255);
end;

function ReadTrustedSubnets(const Text: String; var Subnets: String): Boolean;
var
  Remaining, Entry: String;
  Sep: Integer;
begin
  Result := False;
  Subnets := '';
  Remaining := Trim(Text);
  if Length(Remaining) > 4096 then
    Exit;
  while Remaining <> '' do
  begin
    Sep := Pos(',', Remaining);
    if Sep = 0 then
    begin
      Entry := Remaining;
      Remaining := '';
    end
    else
    begin
      Entry := Trim(Copy(Remaining, 1, Sep - 1));
      Delete(Remaining, 1, Sep);
      Remaining := Trim(Remaining);
      if Remaining = '' then
        Exit;
    end;
    if not ValidCidr(Entry) then
      Exit;
    if Subnets <> '' then
      Subnets := Subnets + ',';
    Subnets := Subnets + Entry;
  end;
  Result := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if ConfigureHost and ((CurPageID = SubnetPage.ID) or (CurPageID = wpReady)) then
  begin
    Result := ReadTrustedSubnets(SubnetPage.Values[0], TrustedSubnets);
    if not Result then
      SuppressibleMsgBox('Use comma-separated IPv4 CIDRs, for example ' + DefaultSubnets + '.',
        mbError, MB_OK, IDOK);
  end;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'TrustedSubnets', TrustedSubnets);
end;

procedure AddRule(Rules: Variant; const Name: String; Protocol: Integer; const Port, RemoteScope: String);
var
  Rule: Variant;
begin
  Rule := CreateOleObject('HNetCfg.FWRule');
  Rule.Name := Name;
  Rule.ApplicationName := ExpandConstant('{app}\PhaholRemote.exe');
  Rule.Protocol := Protocol;
  Rule.LocalPorts := Port;
  Rule.RemoteAddresses := RemoteScope;
  Rule.Profiles := DomainAndPrivateProfiles;
  Rule.Direction := Inbound;
  Rule.Action := Allow;
  Rule.Enabled := True;
  Rules.Add(Rule);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Policy, Rules: Variant;
  RemoteScope: String;
begin
  if CurStep = ssDone then
    AutoInstallSucceeded := True;
  if (CurStep <> ssPostInstall) or not ConfigureHost then
    Exit;
  Policy := CreateOleObject('HNetCfg.FwPolicy2');
  Rules := Policy.Rules;
  RemoteScope := 'LocalSubnet';
  if TrustedSubnets <> '' then
    RemoteScope := RemoteScope + ',' + TrustedSubnets;
  Rules.Remove(HostRule);
  Rules.Remove(ViewerRule);
  try
    AddRule(Rules, HostRule, TcpProtocol, '53540', RemoteScope);
    AddRule(Rules, ViewerRule, UdpProtocol, '53541', RemoteScope);
    if not SaveStringToFile(ExpandConstant('{app}\trusted-subnets.txt'), TrustedSubnets, False) then
      RaiseException('Could not save the trusted subnet list.');
  except
    Log('Network configuration failed: ' + GetExceptionMessage);
    Rules.Remove(HostRule);
    Rules.Remove(ViewerRule);
    RaiseException('Could not configure the trusted subnets and Windows Firewall rules for Phahol Remote.');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Policy, Rules: Variant;
begin
  if CurUninstallStep <> usUninstall then
    Exit;
  Policy := CreateOleObject('HNetCfg.FwPolicy2');
  Rules := Policy.Rules;
  Rules.Remove(HostRule);
  Rules.Remove(ViewerRule);
end;
