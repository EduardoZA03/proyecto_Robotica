# Instalar VigIA en otra PC (guía para Claude Code)

> Claude: sigue estos pasos en orden y verifica cada uno. Responde al usuario en español.
> No escribas ni pidas en el chat la clave del WiFi: el usuario la escribe en `secretos.h`.

## 0. Forma rápida (recomendada)
1. Instalar **Arduino IDE 2.x**, **Python 3** (marcar "Add python.exe to PATH") y **Git**.
2. En la carpeta del repositorio: clic derecho en `instalar.ps1` → **Ejecutar con PowerShell**
   (o `powershell -ExecutionPolicy Bypass -File instalar.ps1`).
   Instala placas AVR y ESP32 3.3.12, librería Servo, librería de la IA, librerías de Python
   y crea `07_vigia_completo\secretos.h` desde la plantilla. Al final muestra un resumen.
3. El usuario llena `secretos.h` (WiFi y `PC_IP`).
4. Si el Arduino no aparece en `arduino-cli board list`: abrir "Install drivers for Arduino"
   (viene con el IDE) o instalar el driver CH340 si la placa es un clon.

Los pasos 1–5 de abajo son lo mismo, hecho a mano (para revisar si algo falla).

## 1. Programas necesarios
| Programa | Para qué | Cómo comprobar |
|---|---|---|
| Arduino IDE 2.x | Trae `arduino-cli` | `arduino-cli.exe` en `%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\` |
| Python 3.10 o más nuevo | Servidor de la PC | `python --version` |
| Git | Clonar el repositorio | `git --version` |
| Proteus 8.17 (opcional) | Simulación del Arduino + servos | Abrir `proteus\VigIA_servos.pdsprj` |

## 2. Placas y librerías de Arduino
```powershell
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
& $cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
& $cli core update-index
& $cli core install arduino:avr
& $cli core install esp32:esp32@3.3.12
& $cli lib install Servo
```
Librería de la IA (modelo FOMO entrenado en Edge Impulse): descomprimir
`librerias\MUCKCITO-project-1_inferencing.zip` en `Documentos\Arduino\libraries\`
(debe quedar `libraries\MUCKCITO-project-1_inferencing\library.properties`).
```powershell
Expand-Archive librerias\MUCKCITO-project-1_inferencing.zip "$([Environment]::GetFolderPath('MyDocuments'))\Arduino\libraries" -Force
```

## 3. Archivo secretos.h (NO está en GitHub)
Copiar `secretos.ejemplo.h` como `07_vigia_completo\secretos.h` (y en `06_captura_dataset\` si se usa).
El usuario escribe el nombre exacto del WiFi (2.4 GHz, respetando mayúsculas) y la clave.
`PC_IP` = la IP que muestra el servidor de Python al arrancar.

## 4. Servidor de la PC
```powershell
cd pc_servidor
python -m pip install -r requirements.txt
python servidor_vigia.py --probar prueba.jpg   # solo para verificar que carga los modelos (ya vienen en pc_servidor\modelos)
python servidor_vigia.py                        # o doble clic en iniciar_servidor.bat
```
- La 1.ª vez Windows pregunta por el firewall: permitir Python en redes **privadas**.
- El perfil de la red (Configuración → Red) debe ser **Privada**; si es Pública, el ESP32 no llega a la PC.
  Ese cambio lo hace el usuario, no Claude.

## 5. Compilar y subir
Arduino Uno (COM que aparezca en `arduino-cli board list`):
```powershell
& $cli compile --fqbn arduino:avr:uno --build-path C:\vigia_b03 03_arduino_servos
& $cli upload -p COMx --fqbn arduino:avr:uno --input-dir C:\vigia_b03 03_arduino_servos
```
ESP32-CAM (se sube a través del Uno en modo programador, ver CLAUDE.md). Compilar tarda ~15 min.
Usar una ruta de compilación corta (rutas largas fallan en Windows):
```powershell
& $cli compile --fqbn esp32:esp32:esp32cam --build-path C:\vigia_build 07_vigia_completo
& $cli upload -p COMx --fqbn esp32:esp32:esp32cam --upload-property upload.speed=115200 --input-dir C:\vigia_build 07_vigia_completo
```
Subida correcta = 4 veces "Hash of data verified" + "Hard resetting".

## 6. Qué NO está en GitHub (a propósito)
- `secretos.h` (clave del WiFi).
- `pc_servidor\fotos\`, `pc_servidor\descartadas\`, `personas.json`: fotos y huellas faciales (datos biométricos).
- Imágenes de `dataset\`.
