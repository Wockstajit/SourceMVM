# Optional offline cache for thumbnail URLs captured from CS2's Workshop API.
# Reads only public Steam artwork; fixture JSON and output remain local.
param([string]$Fixture = (Join-Path $PSScriptRoot 'generated/menu-fixture.json'))
$ErrorActionPreference = 'Stop'
$data = Get-Content -LiteralPath $Fixture -Raw | ConvertFrom-Json
$output = Join-Path $PSScriptRoot 'assets/workshop'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$count = 0
foreach ($map in $data.maps) {
    if ($map.imageUrl -notmatch '^https://images\.steamusercontent\.com/' -or [string]$map.workshop_id -notmatch '^\d+$') { continue }
    $name = [string]$map.workshop_id + '.jpg'
    $destination = Join-Path $output $name
    if (-not (Test-Path -LiteralPath $destination)) {
        Invoke-WebRequest -Uri $map.imageUrl -OutFile $destination -UseBasicParsing -TimeoutSec 20
    }
    $map.imageUrl = 'assets/workshop/' + $name
    $count++
}
[IO.File]::WriteAllText($Fixture, ($data | ConvertTo-Json -Depth 100), (New-Object Text.UTF8Encoding $false))
Write-Output "Cached $count Workshop thumbnails."
