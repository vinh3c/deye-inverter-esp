param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$BuildDir = '',
    [int]$Baud = 460800
)
$ErrorActionPreference = 'Stop'
$flashArgs = @('run', '--python', '3.11', (Join-Path $PSScriptRoot 'project.py'), 'flash', '--port', $Port, '--baud', "$Baud")
if ($BuildDir) { $flashArgs += @('--build-dir', $BuildDir) }
& uv @flashArgs
exit $LASTEXITCODE
