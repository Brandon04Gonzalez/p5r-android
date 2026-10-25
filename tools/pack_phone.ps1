# Make the two files to install on a phone without USB: private\celular\hades.apk and Hades-data.zip.
# The zip holds a top-level Hades/ folder (analysis/*.bin + extract/romfs), stored uncompressed (the data is already
# compressed; storing makes extraction on the phone fast). Extract it into Internal storage. See docs/ANDROID.md.
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$out = "$root\private\celular"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$root\private\apk\hades.apk" "$out\hades.apk" -Force

$zipPath = "$out\Hades-data.zip"
Remove-Item $zipPath -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, "Create")
$add = { param($path, $entry)
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $path, $entry,
        [System.IO.Compression.CompressionLevel]::NoCompression) | Out-Null }
foreach ($m in "main", "sdk") { & $add "$root\private\analysis\$m.bin" "Hades/analysis/$m.bin" }
$base = (Resolve-Path "$root\private\extract\romfs").Path
foreach ($f in Get-ChildItem -Recurse -File $base) {
    & $add $f.FullName ("Hades/extract/romfs/" + $f.FullName.Substring($base.Length + 1).Replace('\', '/'))
}
$zip.Dispose()
Write-Host ("{0} ({1:N2} GB) and {2}" -f $zipPath, ((Get-Item $zipPath).Length / 1GB), "$out\hades.apk")
