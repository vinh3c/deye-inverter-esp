param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$BuildDir = '',
    [int]$Baud = 460800
)
$ErrorActionPreference = 'Stop'
$flashArgs = @('run', '--project', (Split-Path $PSScriptRoot -Parent), 'fw', 'flash', '--port', $Port, '--baud', "$Baud")
if ($BuildDir) { $flashArgs += @('--build-dir', $BuildDir) }
& uv @flashArgs
exit $LASTEXITCODE
