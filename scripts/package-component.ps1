param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repoRoot "VERSION") -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION must use semantic versioning (x.y.z). Found: $version"
}

$x86Dll = Join-Path $repoRoot "bin\$($Configuration)_Win32\foo_crayonshell_bridge.dll"
$x64Dll = Join-Path $repoRoot "bin\$($Configuration)_x64\foo_crayonshell_bridge.dll"

foreach ($requiredDll in @($x86Dll, $x64Dll)) {
    if (-not (Test-Path $requiredDll)) {
        throw "Required architecture build is missing: $requiredDll"
    }
}

$dist = Join-Path $repoRoot "dist"
$stage = Join-Path $dist "stage"
if (Test-Path $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}

$x64Stage = Join-Path $stage "x64"
$licensesStage = Join-Path $stage "licenses"
New-Item -ItemType Directory -Force -Path $x64Stage, $licensesStage | Out-Null

$llhttpLicense = Join-Path $repoRoot "vendor\llhttp\LICENSE"
if (-not (Test-Path $llhttpLicense)) {
    throw "The pinned llhttp MIT license is missing: $llhttpLicense"
}
Copy-Item -LiteralPath $llhttpLicense -Destination (Join-Path $licensesStage "llhttp-MIT.txt") -Force

# foobar2000's multi-architecture package convention:
# - root DLL is the legacy x86 build
# - x64/ DLL replaces the root DLL in x64 foobar2000
Copy-Item -LiteralPath $x86Dll -Destination (Join-Path $stage "foo_crayonshell_bridge.dll") -Force
Copy-Item -LiteralPath $x64Dll -Destination (Join-Path $x64Stage "foo_crayonshell_bridge.dll") -Force

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$packageName = "foo_crayonshell_bridge-$version-x86-x64.fb2k-component"
$packagePath = Join-Path $dist $packageName
if (Test-Path $packagePath) {
    Remove-Item -LiteralPath $packagePath -Force
}

$zip = [System.IO.Compression.ZipFile]::Open(
    $packagePath,
    [System.IO.Compression.ZipArchiveMode]::Create
)
try {
    foreach ($entry in @(
        @{ Source = (Join-Path $stage "foo_crayonshell_bridge.dll"); Name = "foo_crayonshell_bridge.dll" },
        @{ Source = (Join-Path $x64Stage "foo_crayonshell_bridge.dll"); Name = "x64/foo_crayonshell_bridge.dll" },
        @{ Source = (Join-Path $licensesStage "llhttp-MIT.txt"); Name = "licenses/llhttp-MIT.txt" }
    )) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip,
            $entry.Source,
            $entry.Name,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
}
finally {
    $zip.Dispose()
}

$hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
$hashPath = "$packagePath.sha256"
"$hash  $packageName" | Set-Content -LiteralPath $hashPath -Encoding ascii

Write-Host "x86 DLL:  $x86Dll"
Write-Host "x64 DLL:  $x64Dll"
Write-Host "Package:  $packagePath"
Write-Host "SHA256:   $hash"
