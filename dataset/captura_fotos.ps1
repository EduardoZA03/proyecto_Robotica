# VigIA - Descarga fotos de la ESP32-CAM para el dataset
# ------------------------------------------------------------------
# Requiere que la ESP32-CAM tenga cargado 06_captura_dataset y que la PC
# esté en la misma red WiFi.
#
# Uso (PowerShell):
#   powershell -ExecutionPolicy Bypass -File captura_fotos.ps1 -Ip 192.168.1.50
#   (opcional) -Cantidad 120 -Intervalo 1.5 -Carpeta esp32cam
#
# Las fotos se guardan en dataset\<Carpeta>\ como cam_AAAAMMDD_HHMMSS_N.jpg
# Consejos mientras captura: cambiar de distancia (0.5 a 3 m), mirar de
# frente y un poco de lado, distinta luz, varias personas, y algunas
# fotos SIN nadie (sirven como fondo).

param(
    [Parameter(Mandatory = $true)][string]$Ip,
    [int]$Cantidad = 120,
    [double]$Intervalo = 1.5,
    [string]$Carpeta = 'esp32cam'
)

$destino = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) $Carpeta
New-Item -ItemType Directory -Force $destino | Out-Null
$url = "http://$Ip/foto"
$cliente = New-Object Net.WebClient

Write-Host "Descargando $Cantidad fotos de $url cada $Intervalo s -> $destino"
$ok = 0; $fallos = 0
while ($ok -lt $Cantidad) {
    $nombre = 'cam_{0}_{1:D3}.jpg' -f (Get-Date -Format 'yyyyMMdd_HHmmss'), $ok
    try {
        $cliente.DownloadFile($url, (Join-Path $destino $nombre))
        $ok++
        if ($ok % 10 -eq 0) { Write-Host "  $ok / $Cantidad" }
    } catch {
        $fallos++
        Write-Host "  fallo al descargar ($fallos): $($_.Exception.Message)"
        if ($fallos -ge 10) { Write-Host 'Demasiados fallos: revisar la IP y el WiFi'; break }
    }
    Start-Sleep -Milliseconds ([int]($Intervalo * 1000))
}
Write-Host "Listo: $ok fotos en $destino"
