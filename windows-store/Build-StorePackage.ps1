[CmdletBinding()]
param(
    [string]$PackageIdentityName = "AtomicRipper",
    [string]$Publisher = "CN=AtomicRipper",
    [string]$PublisherDisplayName = "AtomicRipper",
    [string]$DisplayName = "AtomicRipper",
    [string]$Version = "0.7.3.0",
    [string]$Description = "A modern secure CD ripper for Windows.",
    [string]$Configuration = "Release",
    [switch]$SkipBuild,
    [switch]$KeepEmbeddedManifest,
    [switch]$Sign,
    [string]$CertificatePath,
    [string]$CertificatePassword
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")
$buildDir = Join-Path $repoRoot "build"
$binDir = Join-Path $buildDir "bin\$Configuration"
$outDir = Join-Path $scriptDir "out"
$stageDir = Join-Path $outDir "stage"
$packagePath = Join-Path $outDir ("AtomicRipper_{0}_x64.msix" -f $Version)

function Find-WindowsSdkTool {
    param([Parameter(Mandatory=$true)][string]$ToolName)

    $sdkRoot = "C:\Program Files (x86)\Windows Kits\10\bin"
    $tool = Get-ChildItem -Path $sdkRoot -Recurse -Filter $ToolName -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\$([regex]::Escape($ToolName))$" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1

    if (-not $tool) {
        throw "Could not find $ToolName. Install the Windows 10/11 SDK."
    }
    return $tool.FullName
}

function Find-CMake {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vsCMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsCMake) { return $vsCMake }

    throw "Could not find cmake.exe."
}

function New-StoreAsset {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][int]$Width,
        [Parameter(Mandatory=$true)][int]$Height,
        [switch]$Wide
    )

    Add-Type -AssemblyName System.Drawing

    $bitmap = New-Object System.Drawing.Bitmap $Width, $Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::FromArgb(16, 24, 32))

    $accent = [System.Drawing.Color]::FromArgb(64, 180, 168)
    $brush = New-Object System.Drawing.SolidBrush $accent
    $textBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)

    if ($Wide) {
        $iconSize = [Math]::Min($Height - 24, 104)
        $iconX = 18
        $iconY = [int](($Height - $iconSize) / 2)
        $graphics.FillEllipse($brush, $iconX, $iconY, $iconSize, $iconSize)
        $font = New-Object System.Drawing.Font "Segoe UI", ([Math]::Max(18, [int]($iconSize * 0.32))), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
        $format = New-Object System.Drawing.StringFormat
        $format.Alignment = [System.Drawing.StringAlignment]::Center
        $format.LineAlignment = [System.Drawing.StringAlignment]::Center
        $graphics.DrawString("AR", $font, $textBrush, (New-Object System.Drawing.RectangleF $iconX, $iconY, $iconSize, $iconSize), $format)

        $titleFont = New-Object System.Drawing.Font "Segoe UI", 28, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
        $graphics.DrawString("AtomicRipper", $titleFont, $textBrush, 138, 52)
        $titleFont.Dispose()
    } else {
        $iconSize = [Math]::Min($Width, $Height) - [Math]::Max(8, [int]([Math]::Min($Width, $Height) * 0.18))
        $iconX = [int](($Width - $iconSize) / 2)
        $iconY = [int](($Height - $iconSize) / 2)
        $graphics.FillEllipse($brush, $iconX, $iconY, $iconSize, $iconSize)
        $fontSize = [Math]::Max(10, [int]($iconSize * 0.34))
        $font = New-Object System.Drawing.Font "Segoe UI", $fontSize, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
        $format = New-Object System.Drawing.StringFormat
        $format.Alignment = [System.Drawing.StringAlignment]::Center
        $format.LineAlignment = [System.Drawing.StringAlignment]::Center
        $graphics.DrawString("AR", $font, $textBrush, (New-Object System.Drawing.RectangleF $iconX, $iconY, $iconSize, $iconSize), $format)
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)

    if ($format) { $format.Dispose() }
    if ($font) { $font.Dispose() }
    $brush.Dispose()
    $textBrush.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

function Set-ExecutableManifest {
    param(
        [Parameter(Mandatory=$true)][string]$ExePath,
        [Parameter(Mandatory=$true)][string]$ManifestPath
    )

    $mt = Find-WindowsSdkTool -ToolName "mt.exe"
    & $mt "-manifest" $ManifestPath "-outputresource:$ExePath;#1"
}

if (-not $SkipBuild) {
    $cmake = Find-CMake
    & $cmake --preset windows-msvc-x64
    & $cmake --build $buildDir --config $Configuration
}

if (-not (Test-Path (Join-Path $binDir "AtomicRipper-gui.exe"))) {
    throw "Missing built GUI executable. Expected: $(Join-Path $binDir 'AtomicRipper-gui.exe')"
}

if (Test-Path $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Get-ChildItem -LiteralPath $binDir -Force |
    Where-Object { $_.Name -ne "atomicripper-tests.exe" } |
    Copy-Item -Destination $stageDir -Recurse -Force

if (-not $KeepEmbeddedManifest) {
    $storeManifest = Join-Path $scriptDir "Application.manifest"
    Set-ExecutableManifest -ExePath (Join-Path $stageDir "AtomicRipper-gui.exe") -ManifestPath $storeManifest
    Set-ExecutableManifest -ExePath (Join-Path $stageDir "atomicripper.exe") -ManifestPath $storeManifest
}

$assetsDir = Join-Path $stageDir "Assets"
$listingAssetsDir = Join-Path $scriptDir "store-listing-assets"
& (Join-Path $scriptDir "Generate-StoreLogos.ps1") -OutputDir $listingAssetsDir | Out-Null
New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null
Copy-Item -LiteralPath (Join-Path $listingAssetsDir "AtomicRipper_Logo_50x50.png") -Destination (Join-Path $assetsDir "StoreLogo.png") -Force
Copy-Item -LiteralPath (Join-Path $listingAssetsDir "AtomicRipper_Logo_44x44.png") -Destination (Join-Path $assetsDir "Square44x44Logo.png") -Force
Copy-Item -LiteralPath (Join-Path $listingAssetsDir "AtomicRipper_Logo_71x71.png") -Destination (Join-Path $assetsDir "Square71x71Logo.png") -Force
Copy-Item -LiteralPath (Join-Path $listingAssetsDir "AtomicRipper_Logo_150x150.png") -Destination (Join-Path $assetsDir "Square150x150Logo.png") -Force
Copy-Item -LiteralPath (Join-Path $listingAssetsDir "AtomicRipper_AppIcon_300x300.png") -Destination (Join-Path $assetsDir "Square310x310Logo.png") -Force
Copy-Item -LiteralPath (Join-Path $listingAssetsDir "AtomicRipper_Wide_310x150.png") -Destination (Join-Path $assetsDir "Wide310x150Logo.png") -Force

$manifest = Get-Content (Join-Path $scriptDir "Package.appxmanifest.in") -Raw
$manifest = $manifest.Replace("@PACKAGE_IDENTITY_NAME@", $PackageIdentityName)
$manifest = $manifest.Replace("@PUBLISHER@", $Publisher)
$manifest = $manifest.Replace("@PUBLISHER_DISPLAY_NAME@", $PublisherDisplayName)
$manifest = $manifest.Replace("@DISPLAY_NAME@", $DisplayName)
$manifest = $manifest.Replace("@VERSION@", $Version)
$manifest = $manifest.Replace("@DESCRIPTION@", $Description)
Set-Content -Path (Join-Path $stageDir "AppxManifest.xml") -Value $manifest -Encoding UTF8

$makeAppx = Find-WindowsSdkTool -ToolName "makeappx.exe"
& $makeAppx pack /d $stageDir /p $packagePath /overwrite

if ($Sign) {
    if (-not $CertificatePath) {
        throw "Pass -CertificatePath when using -Sign."
    }

    $signTool = Find-WindowsSdkTool -ToolName "signtool.exe"
    $args = @("sign", "/fd", "SHA256", "/f", $CertificatePath)
    if ($CertificatePassword) {
        $args += @("/p", $CertificatePassword)
    }
    $args += $packagePath
    & $signTool @args
}

Write-Host ""
Write-Host "Package created:"
Write-Host "  $packagePath"
Write-Host ""
Write-Host "Staged package files:"
Write-Host "  $stageDir"
