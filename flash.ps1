<#
    flash.ps1 - grava a versao atual do firmware na CYD ESP32-2432S028,
                no Windows.

    Uso: conecte a placa no USB e rode:
        .\flash.ps1

    Acha a porta sozinho (espera ate 30s se a placa ainda nao apareceu),
    compila e grava. Equivalente ao flash.sh; ver docs/HARDWARE-CYD.md para
    os pre-requisitos.
#>
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Get-BoardPort {
    $dev = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
           Where-Object { $_.FriendlyName -match 'CH340|CP210|CH910' } |
           Select-Object -First 1
    if ($dev -and $dev.FriendlyName -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

$port = Get-BoardPort
if (-not $port) {
    Write-Host '==> nenhuma placa na USB - conecte o device (aguardando ate 30s)...'
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 1
        $port = Get-BoardPort
        if ($port) { break }
    }
}
if (-not $port) {
    throw 'nenhuma porta serial apareceu. A placa esta conectada?'
}

Write-Host "==> placa encontrada em $port"
Write-Host '==> compilando e gravando a versao atual...'
& "$PSScriptRoot\firmware\claude_stick\build.ps1" upload $port
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host ''
Write-Host '==> pronto! O device reinicia sozinho (vai pedir o PIN na tela).'
