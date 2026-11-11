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
AppMutex=Local\PhaholRemote
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
Root: HKLM64; Subkey: "SOFTWARE\Phahol Remote"; ValueType: dword; ValueName: "AllowRemotePower"; ValueData: "1"; Tasks: remotepower; Flags: uninsdeletevalue
Root: HKLM64; Subkey: "SOFTWARE\Phahol Remote"; ValueType: dword; ValueName: "AllowRemotePower"; ValueData: "0"; Tasks: not remotepower; Flags: uninsdeletevalue

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

var
  SubnetPage: TInputQueryWizardPage;
  TrustedSubnets: String;

procedure InitializeWizard;
var
  SavedSubnets: String;
begin
  SubnetPage := CreateInputQueryPage(wpSelectDir, 'Trusted networks',
    'Allow connections across routed VLANs',
    'Enter trusted IPv4 CIDRs separated by commas. Leave blank for local subnet only.');
  SubnetPage.Add('Additional trusted IPv4 subnets:', False);
  SavedSubnets := GetPreviousData('TrustedSubnets', DefaultSubnets);
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
  if (CurPageID = SubnetPage.ID) or (CurPageID = wpReady) then
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
  if CurStep <> ssPostInstall then
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
