# Instala todo lo necesario para VigIA en una PC nueva (Windows)
# Uso: clic derecho -> "Ejecutar con PowerShell", o: powershell -ExecutionPolicy Bypass -File instalar.ps1

$ErrorActionPreference = "Continue"
$aqui = Split-Path -Parent $MyInvocation.MyCommand.Path   # carpeta del repositorio
$urlEsp32 = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"   # índice de placas ESP32
$problemas = @()                                         # lista de pasos que fallaron

# Muestra un título de paso
function Paso($texto) { Write-Host "`n=== $texto ===" -ForegroundColor Cyan }

# Busca arduino-cli (Arduino IDE 2.x o instalado aparte)
function Buscar-ArduinoCli {
    $rutas = @(
        "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
        "$env:ProgramFiles\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
    )
    foreach ($r in $rutas) { if (Test-Path $r) { return $r } }
    $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

Paso "1. Arduino IDE / arduino-cli"
$cli = Buscar-ArduinoCli
if (-not $cli) {
    Write-Host "No se encontró Arduino IDE. Instálalo desde https://www.arduino.cc/en/software y vuelve a ejecutar." -ForegroundColor Red
    $problemas += "Falta Arduino IDE"
} else {
    Write-Host "arduino-cli: $cli"
    & $cli core update-index --additional-urls $urlEsp32
    & $cli core install arduino:avr --additional-urls $urlEsp32
    & $cli core install esp32:esp32@3.3.12 --additional-urls $urlEsp32
    if ($LASTEXITCODE -ne 0) { $problemas += "No se instaló la placa ESP32 3.3.12" }

    Paso "2. Librería Servo"
    & $cli lib install Servo
    if ($LASTEXITCODE -ne 0) { $problemas += "No se instaló la librería Servo" }

    Paso "3. Librería de la IA (Edge Impulse / FOMO)"
    $sketchbook = & $cli config get directories.user | Where-Object { $_ -and $_.Trim() } | Select-Object -First 1   # carpeta de librerías de Arduino
    if ($sketchbook) { $sketchbook = $sketchbook.Trim() }
    if (-not $sketchbook -or -not (Test-Path $sketchbook -IsValid)) { $sketchbook = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Arduino" }
    $libs = Join-Path $sketchbook "libraries"
    New-Item -ItemType Directory -Force $libs | Out-Null
    Expand-Archive (Join-Path $aqui "librerias\MUCKCITO-project-1_inferencing.zip") $libs -Force
    if (Test-Path (Join-Path $libs "MUCKCITO-project-1_inferencing\library.properties")) {
        Write-Host "Librería de la IA instalada en $libs"
    } else { $problemas += "No se descomprimió la librería de la IA" }
}

Paso "4. Python y librerías"
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) {
    Write-Host "No se encontró Python. Instálalo desde https://www.python.org (marca 'Add python.exe to PATH')." -ForegroundColor Red
    $problemas += "Falta Python"
} else {
    python --version
    python -m pip install -r (Join-Path $aqui "pc_servidor\requirements.txt")
    if ($LASTEXITCODE -ne 0) { $problemas += "Falló pip install" }
    Push-Location (Join-Path $aqui "pc_servidor")
    python servidor_vigia.py --probar no_existe.jpg
    Pop-Location
}

Paso "5. secretos.h (datos del WiFi)"
$secretos = Join-Path $aqui "07_vigia_completo\secretos.h"
if (Test-Path $secretos) {
    Write-Host "secretos.h ya existe."
} else {
    Copy-Item (Join-Path $aqui "secretos.ejemplo.h") $secretos
    Write-Host "Se creó 07_vigia_completo\secretos.h: ábrelo y escribe el nombre y la clave de tu WiFi y la IP de la PC." -ForegroundColor Yellow
}

Paso "Resumen"
if ($problemas.Count -eq 0) {
    Write-Host "Todo instalado. Siguiente: llenar secretos.h, compilar y subir (ver INSTALAR.md)." -ForegroundColor Green
} else {
    Write-Host "Revisar:" -ForegroundColor Red
    $problemas | ForEach-Object { Write-Host " - $_" -ForegroundColor Red }
}
Read-Host "`nPresiona Enter para cerrar"
