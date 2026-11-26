param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repoRoot "VERSION") -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION must use semantic versioning (x.y.z). Found: $version"
}

$dll = Join-Path $repoRoot "bin\$($Configuration)_$($Platform)\foo_crayonshell_bridge.dll"
if (-not (Test-Path $dll)) {
    throw "Component DLL not found: $dll"
}

$dist = Join-Path $repoRoot "dist"
$stage = Join-Path $dist "stage"
if (Test-Path $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# This is an x64-only package, so the architecture-specific DLL lives at archive root.
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage "foo_crayonshell_bridge.dll") -Force

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$packageName = "foo_crayonshell_bridge-$version-x64.fb2k-component"
$packagePath = Join-Path $dist $packageName
if (Test-Path $packagePath) { Remove-Item -LiteralPath $packagePath -Force }

$zip = [System.IO.Compression.ZipFile]::Open(
    $packagePath,
    [System.IO.Compression.ZipArchiveMode]::Create
)
try {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $zip,
        (Join-Path $stage "foo_crayonshell_bridge.dll"),
        "foo_crayonshell_bridge.dll",
        [System.IO.Compression.CompressionLevel]::Optimal
    ) | Out-Null
}
finally {
    $zip.Dispose()
}

$hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
$hashPath = "$packagePath.sha256"
"$hash  $packageName" | Set-Content -LiteralPath $hashPath -Encoding ascii

Write-Host "Package: $packagePath"
Write-Host "SHA256:  $hash"
