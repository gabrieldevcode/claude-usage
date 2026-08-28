<#
    build.ps1 - build / upload / monitor do Claude Usage Stick na CYD
                ESP32-2432S028, no Windows.

    Uso:
        .\build.ps1                  # compila
        .\build.ps1 upload           # compila + grava (acha a porta sozinho)
        .\build.ps1 upload COM8      # compila + grava na porta indicada
        .\build.ps1 monitor COM8     # serial monitor a 115200

    Nao precisa instalar arduino-cli separado: o Arduino IDE ja traz um em
    resources\app\lib\backend\resources\. Se houver um arduino-cli no PATH,
    ele tem preferencia.

    Equivalente ao build.sh; ver os comentarios de la para o porque de cada
    opcao do FQBN.
#>
param(
    [ValidateSet('build', 'upload', 'monitor')]
    [string]$Command = 'build',
    [string]$Port
)

$ErrorActionPreference = 'Stop'
$SketchDir = $PSScriptRoot

$Fqbn = 'esp32:esp32:esp32:FlashSize=4M,PartitionScheme=custom,CPUFreq=240,FlashFreq=80,FlashMode=qio'
$LvFlags = "-DLV_CONF_INCLUDE_SIMPLE -I$SketchDir"

function Get-ArduinoCli {
    $onPath = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $bundled = Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
    if (Test-Path $bundled) { return $bundled }
    throw 'arduino-cli nao encontrado (nem no PATH, nem dentro do Arduino IDE).'
}

function Get-BoardPort {
    # A CYD usa um CH340; procuramos por ele em vez de chutar um numero de COM.
    $dev = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
           Where-Object { $_.FriendlyName -match 'CH340|CP210|CH910' } |
           Select-Object -First 1
    if ($dev -and $dev.FriendlyName -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

$cli = Get-ArduinoCli

if ($Command -ne 'build' -and -not $Port) {
    $Port = Get-BoardPort
    if (-not $Port) { throw 'nenhuma placa CH340/CP210 conectada. Passe a porta como argumento.' }
    Write-Host "==> porta encontrada: $Port"
}

$compileArgs = @(
    'compile',
    '--fqbn', $Fqbn,
    '--build-property', "compiler.cpp.extra_flags=$LvFlags",
    '--build-property', "compiler.c.extra_flags=$LvFlags",
    '--build-property', "compiler.S.extra_flags=$LvFlags"
)

switch ($Command) {
    'monitor' {
        & $cli monitor -p $Port -c baudrate=115200
    }
    'build' {
        Write-Host "==> compilando ($Fqbn)"
        & $cli @compileArgs $SketchDir
    }
    'upload' {
        # compile --upload faz tudo num passo; upload puro nao aceita
        # --build-property, e sem ele o lv_conf.h nao e achado.
        Write-Host "==> compilando + gravando em $Port ($Fqbn)"
        & $cli @compileArgs '--upload' '-p' $Port $SketchDir
    }
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
