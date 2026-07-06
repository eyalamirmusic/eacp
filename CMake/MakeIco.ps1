# Packs a single PNG master into a multi-resolution .ico for the ICON
# resource embedded in eacp app executables (see eacp_set_app_icon).
# Every entry is stored PNG-compressed, which every supported Windows
# version (Vista+) reads; System.Drawing does the resizing, so no
# third-party tooling is needed at build time.
param(
    [Parameter(Mandatory)] [string] $Source,
    [Parameter(Mandatory)] [string] $Output
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sizes = 16, 24, 32, 48, 64, 128, 256
$master = [System.Drawing.Image]::FromFile((Resolve-Path $Source))

$entries = foreach ($size in $sizes)
{
    $bitmap = New-Object System.Drawing.Bitmap $size, $size
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.InterpolationMode = 'HighQualityBicubic'
    $graphics.DrawImage($master, 0, 0, $size, $size)
    $graphics.Dispose()

    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    , $stream.ToArray()
}

$master.Dispose()

$writer = New-Object System.IO.BinaryWriter ([System.IO.File]::Create($Output))

# ICONDIR: reserved, type 1 (icon), image count.
$writer.Write([uint16] 0)
$writer.Write([uint16] 1)
$writer.Write([uint16] $sizes.Count)

# ICONDIRENTRY per image; a width/height byte of 0 means 256.
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++)
{
    $size = $sizes[$i]
    $writer.Write([byte] ($size -band 0xFF))
    $writer.Write([byte] ($size -band 0xFF))
    $writer.Write([byte] 0)
    $writer.Write([byte] 0)
    $writer.Write([uint16] 1)
    $writer.Write([uint16] 32)
    $writer.Write([uint32] $entries[$i].Length)
    $writer.Write([uint32] $offset)
    $offset += $entries[$i].Length
}

foreach ($entry in $entries)
{
    $writer.Write($entry)
}

$writer.Dispose()
Write-Host "wrote $Output"
