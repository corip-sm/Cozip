param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,

    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Store", "Deflate")]
    [string]$Mode,

    [Parameter(Mandatory = $true)]
    [ValidateSet("7zip", "Bandizip")]
    [string]$Tool
)

function Resolve-ExternalToolPath {
    param([string]$Name)

    if ($Name -eq "7zip") {
        $candidates = @(
            "7z",
            "C:\NewGen\Rebirth\Dll\7z.exe",
            "C:\Program Files\7-Zip\7z.exe",
            "C:\Program Files (x86)\7-Zip\7z.exe"
        )
    }
    else {
        $candidates = @(
            "bz",
            "C:\Program Files\Bandizip\bz.exe",
            "C:\Program Files (x86)\Bandizip\bz.exe"
        )
    }

    foreach ($candidate in $candidates) {
        try {
            $command = Get-Command $candidate -ErrorAction Stop
            return $command.Source
        }
        catch {
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }

    return $null
}

$toolPath = Resolve-ExternalToolPath -Name $Tool
if (-not $toolPath) {
    Write-Host "SKIP: tool not found: $Tool"
    exit 100
}

if (Test-Path -LiteralPath $ArchivePath) {
    Remove-Item -LiteralPath $ArchivePath -Force
}

Push-Location -LiteralPath $SourceRoot
try {
    if ($Tool -eq "7zip") {
        $level = if ($Mode -eq "Store") { "-mx=0" } else { "-mx=5" }
        & $toolPath a -tzip $level $ArchivePath .
        exit $LASTEXITCODE
    }
    else {
        $level = if ($Mode -eq "Store") { "-l:0" } else { "-l:5" }
        & $toolPath c -fmt:zip $level -r $ArchivePath .
        exit $LASTEXITCODE
    }
}
finally {
    Pop-Location
}
