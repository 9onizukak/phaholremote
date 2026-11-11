[CmdletBinding()]
param(
    [switch]$Uninstall,
    [AllowEmptyString()]
    [string]$TrustedSubnets = '192.168.0.0/16,172.16.0.0/16'
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Open Windows PowerShell as administrator, then run this script again.'
}

$installDir = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Phahol Remote'
$exe = Join-Path $installDir 'PhaholRemote.exe'
$subnetFile = Join-Path $installDir 'trusted-subnets.txt'
$startupShortcut = Join-Path ([Environment]::GetFolderPath('CommonStartup')) 'Phahol Remote.lnk'
$menuShortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'Phahol Remote.lnk'
$rules = @(
    @{ Name = 'PhaholRemote-Host-TCP'; DisplayName = 'Phahol Remote Host TCP'; Protocol = 'TCP'; LocalPort = 53540 }
    @{ Name = 'PhaholRemote-Viewer-UDP'; DisplayName = 'Phahol Remote Viewer UDP'; Protocol = 'UDP'; LocalPort = 53541 }
)

function Remove-InstalledRules {
    foreach ($rule in $rules) {
        Get-NetFirewallRule -Name $rule.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    }
}

if ($Uninstall) {
    if (Test-Path -LiteralPath $exe) {
        Remove-Item -LiteralPath $exe
    }
    foreach ($file in $startupShortcut, $menuShortcut, $subnetFile) {
        if (Test-Path -LiteralPath $file) {
            Remove-Item -LiteralPath $file
        }
    }
    Remove-InstalledRules
    Write-Host 'Phahol Remote was uninstalled.'
    return
}

$subnets = @()
if ($TrustedSubnets.Trim()) {
    $octet = '(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])'
    $cidr = "\A(?:$octet\.){3}$octet/(?:[1-9]|[12][0-9]|3[0-2])\z"
    foreach ($entry in $TrustedSubnets.Split(',')) {
        $subnet = $entry.Trim()
        if ($subnet -notmatch $cidr) {
            throw "Invalid IPv4 CIDR: '$subnet'."
        }
        $subnets += $subnet
    }
}
$subnetText = $subnets -join ','
if ($subnetText.Length -gt 4096) { throw 'The trusted subnet list is too long.' }

$source = Join-Path $PSScriptRoot 'PhaholRemote.exe'
if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
    $source = Join-Path $PSScriptRoot 'build\PhaholRemote.exe'
}
if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
    throw 'PhaholRemote.exe is missing. Extract the full package or build the executable first.'
}

New-Item -ItemType Directory -Path $installDir -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $exe -Force
Remove-InstalledRules
try {
    foreach ($rule in $rules) {
        New-NetFirewallRule @rule -Direction Inbound -Action Allow -Profile Domain,Private `
            -RemoteAddress (@('LocalSubnet') + $subnets) -Program $exe | Out-Null
    }
    [IO.File]::WriteAllText($subnetFile, $subnetText, [Text.Encoding]::ASCII)
} catch {
    Remove-InstalledRules
    throw
}

$shell = New-Object -ComObject WScript.Shell
foreach ($shortcutPath in $startupShortcut, $menuShortcut) {
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $exe
    $shortcut.WorkingDirectory = $installDir
    $isStartup = $shortcutPath -eq $startupShortcut
    $shortcut.WindowStyle = 1
    $shortcut.Arguments = if ($isStartup) { '--tray' } else { '' }
    $shortcut.Save()
}

Write-Host 'Installed. Open Phahol Remote from the Start menu to use it now.'
Write-Host 'The app starts in the system tray at the next Windows sign-in.'
