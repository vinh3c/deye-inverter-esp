param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$BuildDir = (Join-Path $PSScriptRoot '../build'),
    [int]$Baud = 460800
)
$ErrorActionPreference = 'Stop'
$buildPath = (Resolve-Path -LiteralPath $BuildDir).Path
if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'flasher_args.json'))) {
    throw 'Build first: bash scripts/build.sh'
}
# IDF writes the complete offsets/options; never duplicate hardcoded offsets here.
$flash = Get-Content -Raw -LiteralPath (Join-Path $buildPath 'flasher_args.json') | ConvertFrom-Json
$flashArgs = @('--chip', 'esp32c3', '--port', $Port, '--baud', "$Baud", '--before', 'default_reset', '--after', 'hard_reset', 'write_flash')
foreach ($arg in $flash.write_flash_args) { $flashArgs += [string]$arg }
foreach ($file in $flash.flash_files.PSObject.Properties) {
    $flashArgs += $file.Name
    $flashArgs += Join-Path $buildPath $file.Value
}
Push-Location $buildPath
try {
    uv run --with 'esptool==4.8.1' python -m esptool @flashArgs
    if ($LASTEXITCODE -ne 0) { throw "Flash failed ($LASTEXITCODE)" }
} finally { Pop-Location }
