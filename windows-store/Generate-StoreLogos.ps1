[CmdletBinding()]
param(
    [string]$OutputDir = "$PSScriptRoot\store-listing-assets"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

function New-Canvas {
    param([int]$Width, [int]$Height)
    $bitmap = New-Object System.Drawing.Bitmap $Width, $Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    return [pscustomobject]@{ Bitmap = $bitmap; Graphics = $graphics }
}

function New-Brush {
    param([int]$R, [int]$G, [int]$B, [int]$A = 255)
    return New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($A, $R, $G, $B))
}

function New-Pen {
    param([int]$R, [int]$G, [int]$B, [float]$Width, [int]$A = 255)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb($A, $R, $G, $B)), $Width
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    return $pen
}

function Fill-Background {
    param($Graphics, [int]$Width, [int]$Height)

    $rect = New-Object System.Drawing.Rectangle 0, 0, $Width, $Height
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect,
        ([System.Drawing.Color]::FromArgb(2, 8, 5)),
        ([System.Drawing.Color]::FromArgb(7, 22, 13)),
        35
    $Graphics.FillRectangle($bg, $rect)
    $bg.Dispose()

    $gridPen = New-Pen 24 210 107 ([Math]::Max(1, $Width / 900)) 32
    $step = [Math]::Max(36, [int]($Width / 12))
    for ($x = -$Width; $x -lt $Width * 2; $x += $step) {
        $Graphics.DrawLine($gridPen, $x, 0, $x + $Height, $Height)
    }
    $gridPen.Dispose()
}

function Draw-AtomLogo {
    param(
        $Graphics,
        [float]$CenterX,
        [float]$CenterY,
        [float]$Size,
        [switch]$IncludeDisc
    )

    $cyan = New-Pen 24 210 107 ([Math]::Max(2, $Size * 0.035))
    $teal = New-Pen 140 255 181 ([Math]::Max(2, $Size * 0.018)) 190
    $whiteBrush = New-Brush 226 255 239
    $coreBrush = New-Brush 24 210 107
    $discPen = New-Pen 200 255 219 ([Math]::Max(2, $Size * 0.018)) 190
    $slashPen = New-Pen 24 210 107 ([Math]::Max(2, $Size * 0.026)) 230

    $Graphics.TranslateTransform($CenterX, $CenterY)

    if ($IncludeDisc) {
        $r = $Size * 0.47
        $Graphics.DrawEllipse($discPen, -$r, -$r, $r * 2, $r * 2)
        $Graphics.DrawEllipse($teal, -$r * 0.20, -$r * 0.20, $r * 0.40, $r * 0.40)
        $Graphics.DrawLine($slashPen, $r * 0.22, -$r * 0.58, $r * 0.74, -$r * 0.16)
    }

    foreach ($angle in @(0, 60, -60)) {
        $Graphics.RotateTransform($angle)
        $w = $Size * 0.88
        $h = $Size * 0.34
        $Graphics.DrawEllipse($cyan, -$w / 2, -$h / 2, $w, $h)
        $Graphics.RotateTransform(-$angle)
    }

    $core = $Size * 0.13
    $Graphics.FillEllipse($coreBrush, -$core / 2, -$core / 2, $core, $core)
    $Graphics.FillEllipse($whiteBrush, $Size * 0.18, -$Size * 0.032, $Size * 0.07, $Size * 0.07)

    $Graphics.ResetTransform()
    $cyan.Dispose()
    $teal.Dispose()
    $whiteBrush.Dispose()
    $coreBrush.Dispose()
    $discPen.Dispose()
    $slashPen.Dispose()
}

function Draw-Wordmark {
    param($Graphics, [string]$Text, [float]$X, [float]$Y, [float]$Size)

    $font = New-Object System.Drawing.Font "Segoe UI", $Size, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $brush = New-Brush 215 255 230
    $Graphics.DrawString($Text, $font, $brush, $X, $Y)
    $font.Dispose()
    $brush.Dispose()
}

function Save-Logo {
    param(
        [string]$FileName,
        [int]$Width,
        [int]$Height,
        [string]$Mode
    )

    $canvas = New-Canvas $Width $Height
    $g = $canvas.Graphics
    Fill-Background $g $Width $Height

    if ($Mode -eq "wide") {
        Draw-AtomLogo $g ($Height * 0.58) ($Height * 0.50) ($Height * 0.68) -IncludeDisc
        Draw-Wordmark $g "AtomicRipper" ($Height * 1.06) ($Height * 0.34) ($Height * 0.18)
        Draw-Wordmark $g "CD Ripper" ($Height * 1.06) ($Height * 0.52) ($Height * 0.12)
    } elseif ($Mode -eq "poster") {
        Draw-AtomLogo $g ($Width * 0.50) ($Height * 0.32) ($Width * 0.54) -IncludeDisc
        Draw-Wordmark $g "AtomicRipper" ($Width * 0.13) ($Height * 0.60) ($Width * 0.088)
        Draw-Wordmark $g "CD Ripper" ($Width * 0.13) ($Height * 0.675) ($Width * 0.064)
    } elseif ($Mode -eq "square-title") {
        Draw-AtomLogo $g ($Width * 0.50) ($Height * 0.40) ($Width * 0.48) -IncludeDisc
        Draw-Wordmark $g "AtomicRipper" ($Width * 0.18) ($Height * 0.69) ($Width * 0.066)
        Draw-Wordmark $g "CD Ripper" ($Width * 0.18) ($Height * 0.765) ($Width * 0.046)
    } else {
        Draw-AtomLogo $g ($Width / 2) ($Height / 2) ([Math]::Min($Width, $Height) * 0.70) -IncludeDisc
    }

    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    $path = Join-Path $OutputDir $FileName
    $canvas.Bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $canvas.Bitmap.Dispose()
    return $path
}

$created = @()
$created += Save-Logo "AtomicRipper_AppIcon_300x300.png" 300 300 "icon"
$created += Save-Logo "AtomicRipper_Logo_44x44.png" 44 44 "icon"
$created += Save-Logo "AtomicRipper_Logo_150x150.png" 150 150 "icon"
$created += Save-Logo "AtomicRipper_Logo_71x71.png" 71 71 "icon"
$created += Save-Logo "AtomicRipper_Logo_50x50.png" 50 50 "icon"
$created += Save-Logo "AtomicRipper_Square_2100x2100.png" 2100 2100 "square-title"
$created += Save-Logo "AtomicRipper_Poster_1440x2160.png" 1440 2160 "poster"
$created += Save-Logo "AtomicRipper_Wide_3840x2160.png" 3840 2160 "wide"
$created += Save-Logo "AtomicRipper_Wide_310x150.png" 310 150 "wide"

$created | ForEach-Object { Write-Output $_ }
