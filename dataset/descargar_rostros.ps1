# VigIA - Descarga de imágenes con rostros desde Open Images (Google)
# ------------------------------------------------------------------
# Descarga N imágenes del conjunto de validación de Open Images que
# tienen la clase "Human face" ya etiquetada con recuadros, las reduce
# de tamaño y genera el archivo bounding_boxes.labels que Edge Impulse
# lee automáticamente al subir la carpeta.
#
# Uso (PowerShell):
#   powershell -ExecutionPolicy Bypass -File descargar_rostros.ps1 -Cantidad 300
#   Lote extra sin repetir las anteriores:
#   powershell -ExecutionPolicy Bypass -File descargar_rostros.ps1 -Cantidad 700 -Carpeta rostros_extra -Excluir rostros
#
# Resultado:
#   rostros\                       imágenes .jpg + bounding_boxes.labels
#   rostros\atribuciones.csv       autor y licencia de cada imagen (CC BY 2.0)
#
# Fuente: Open Images Dataset V7 - https://storage.googleapis.com/openimages/web/index.html
# Anotaciones: CC BY 4.0 (Google). Imágenes: CC BY 2.0 (ver atribuciones.csv).

param(
    [int]$Cantidad = 300,
    [int]$LadoMax = 480,          # lado mayor de la imagen guardada (px)
    [double]$RostroMin = 0.10,    # ancho mínimo del rostro (fracción de la imagen)
    [int]$MaxRostros = 4,         # descartar fotos con multitudes
    [int]$Semilla = 42,
    [string]$Carpeta = 'rostros',    # carpeta de salida (dentro de dataset\)
    [string[]]$Excluir = @()         # carpetas cuyas imágenes NO se repiten
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$base   = Split-Path -Parent $MyInvocation.MyCommand.Path
$cache  = Join-Path $base '_cache'
$salida = Join-Path $base $Carpeta
New-Item -ItemType Directory -Force $cache, $salida | Out-Null

$csvBoxes = Join-Path $cache 'validation-annotations-bbox.csv'
$csvInfo  = Join-Path $cache 'validation-images-with-rotation.csv'
$urlBoxes = 'https://storage.googleapis.com/openimages/v5/validation-annotations-bbox.csv'
$urlInfo  = 'https://storage.googleapis.com/openimages/2018_04/validation/validation-images-with-rotation.csv'

foreach ($par in @(@($urlBoxes, $csvBoxes), @($urlInfo, $csvInfo))) {
    if (-not (Test-Path $par[1])) {
        Write-Host "Descargando $(Split-Path -Leaf $par[1])..."
        Invoke-WebRequest -Uri $par[0] -OutFile $par[1] -UseBasicParsing
    }
}

# ---- 1. Leer recuadros de "Human face" (/m/0dzct) ----
Write-Host 'Leyendo anotaciones...'
$rostros = @{}       # ImageID -> lista de recuadros
$descartadas = @{}   # imágenes con rostros dibujados o grupales
$lector = [IO.StreamReader]::new($csvBoxes)
$null = $lector.ReadLine()   # cabecera
while ($null -ne ($linea = $lector.ReadLine())) {
    if ($linea.IndexOf('/m/0dzct') -lt 0) { continue }
    # ImageID,Source,LabelName,Confidence,XMin,XMax,YMin,YMax,IsOccluded,IsTruncated,IsGroupOf,IsDepiction,IsInside
    $c = $linea.Split(',')
    if ($c[2] -ne '/m/0dzct') { continue }
    $id = $c[0]
    if ($c[10] -eq '1' -or $c[11] -eq '1') { $descartadas[$id] = $true; continue }
    if (-not $rostros.ContainsKey($id)) { $rostros[$id] = New-Object System.Collections.ArrayList }
    [void]$rostros[$id].Add([double[]]@($c[4], $c[5], $c[6], $c[7]))
}
$lector.Close()

# ---- 2. Filtrar imágenes útiles para FOMO ----
$info = @{}
Import-Csv $csvInfo | ForEach-Object { $info[$_.ImageID] = $_ }

$candidatas = foreach ($id in $rostros.Keys) {
    if ($descartadas.ContainsKey($id)) { continue }
    $cajas = $rostros[$id]
    if ($cajas.Count -gt $MaxRostros) { continue }
    $chicas = @($cajas | Where-Object { ($_[1] - $_[0]) -lt $RostroMin })
    if ($chicas.Count -gt 0) { continue }   # algún rostro muy pequeño para 96x96
    $meta = $info[$id]
    # Rotation viene como "0.0"; vacío = desconocida, también se descarta
    if (-not $meta -or $meta.Rotation -ne '0.0') { continue }
    $id
}
$candidatas = @($candidatas | Sort-Object)
Write-Host "Imágenes que cumplen los filtros: $($candidatas.Count)"

$yaUsadas = @{}
foreach ($dir in $Excluir) {
    Get-ChildItem (Join-Path $base $dir) -Filter *.jpg -ErrorAction SilentlyContinue |
        ForEach-Object { $yaUsadas[$_.BaseName] = $true }
}
if ($yaUsadas.Count) { Write-Host "Se excluyen $($yaUsadas.Count) imágenes ya descargadas" }

$rng = [Random]::new($Semilla)
$elegidas = $candidatas | Where-Object { -not $yaUsadas.ContainsKey($_) } |
    Sort-Object { $rng.Next() } | Select-Object -First $Cantidad

# ---- 3. Descargar, reducir y anotar ----
$etiquetas = [ordered]@{}
$atrib = New-Object System.Collections.ArrayList
$cliente = New-Object Net.WebClient
$codec = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$params = New-Object Drawing.Imaging.EncoderParameters 1
$params.Param[0] = New-Object Drawing.Imaging.EncoderParameter ([Drawing.Imaging.Encoder]::Quality), 85L

$n = 0
foreach ($id in $elegidas) {
    $n++
    $archivo = "$id.jpg"
    $destino = Join-Path $salida $archivo
    try {
        $bytes = $cliente.DownloadData("https://s3.amazonaws.com/open-images-dataset/validation/$id.jpg")
        $ms = New-Object IO.MemoryStream (, $bytes)
        $img = [Drawing.Image]::FromStream($ms)
        $escala = [Math]::Min(1.0, $LadoMax / [Math]::Max($img.Width, $img.Height))
        $w = [int][Math]::Round($img.Width * $escala)
        $h = [int][Math]::Round($img.Height * $escala)
        $bmp = New-Object Drawing.Bitmap $w, $h
        $g = [Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($img, 0, 0, $w, $h)
        $bmp.Save($destino, $codec, $params)
        $g.Dispose(); $bmp.Dispose(); $img.Dispose(); $ms.Dispose()
    } catch {
        Write-Host "  [$n] $id - no se pudo descargar, se omite"
        continue
    }

    $cajas = foreach ($b in $rostros[$id]) {
        [ordered]@{
            label  = 'rostro'
            x      = [int][Math]::Round($b[0] * $w)
            y      = [int][Math]::Round($b[2] * $h)
            width  = [int][Math]::Round(($b[1] - $b[0]) * $w)
            height = [int][Math]::Round(($b[3] - $b[2]) * $h)
        }
    }
    $etiquetas[$archivo] = @($cajas)

    $meta = $info[$id]
    [void]$atrib.Add([pscustomobject]@{
        archivo  = $archivo
        autor    = $meta.Author
        licencia = $meta.License
        original = $meta.OriginalURL
    })
    if ($n % 25 -eq 0) { Write-Host "  $n / $($elegidas.Count)" }
}

# ---- 4. Archivos para Edge Impulse ----
$json = [ordered]@{
    version        = 1
    type           = 'bounding-box-labels'
    boundingBoxes  = $etiquetas
} | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $salida 'bounding_boxes.labels'), $json, (New-Object Text.UTF8Encoding $false))
$atrib | Export-Csv (Join-Path $salida 'atribuciones.csv') -NoTypeInformation -Encoding UTF8

$totalRostros = ($etiquetas.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
Write-Host "Listo: $($etiquetas.Count) imágenes, $totalRostros rostros etiquetados en $salida"
