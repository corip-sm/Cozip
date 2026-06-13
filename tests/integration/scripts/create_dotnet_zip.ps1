param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,

    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Store", "Deflate")]
    [string]$Mode
)

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (Test-Path -LiteralPath $ArchivePath) {
    Remove-Item -LiteralPath $ArchivePath -Force
}

$compressionLevel = if ($Mode -eq "Store") {
    [System.IO.Compression.CompressionLevel]::NoCompression
} else {
    [System.IO.Compression.CompressionLevel]::Optimal
}

$archive = [System.IO.Compression.ZipFile]::Open(
    $ArchivePath,
    [System.IO.Compression.ZipArchiveMode]::Create)

try {
    $resolvedRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
    $files = Get-ChildItem -LiteralPath $SourceRoot -Recurse -File
    foreach ($file in $files) {
        $relativePath = $file.FullName.Substring($resolvedRoot.Length).TrimStart('\', '/')
        $entryName = $relativePath.Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            $file.FullName,
            $entryName,
            $compressionLevel) | Out-Null
    }
}
finally {
    $archive.Dispose()
}
