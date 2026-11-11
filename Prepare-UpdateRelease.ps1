param(
    [string]$Installer = (Join-Path $PSScriptRoot 'build/PhaholRemote-Setup.exe')
)

$ErrorActionPreference = 'Stop'
$versionHeader = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'src/version.h') -Raw
if ($versionHeader -notmatch '(?m)^#define PHAHOL_REMOTE_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"\s*$') {
    throw 'Could not read the release version from src/version.h.'
}
$version = $Matches[1]
$setup = Get-Item -LiteralPath $Installer
if ($setup.VersionInfo.ProductVersion.Trim() -ne $version) {
    throw "Build Setup $version before generating update assets."
}

$release = New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot "build/release-v$version")
$asset = Join-Path $release.FullName 'PhaholRemote-Setup.exe'
if ($setup.FullName -ne $asset) {
    Copy-Item -LiteralPath $setup.FullName -Destination $asset -Force
}
$hash = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $release.FullName 'phahol-update.txt'), "$version`n$hash`n")
Write-Output "Upload both files from $($release.FullName) to the GitHub Release tagged v$version."
