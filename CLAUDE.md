# VigIA — Contexto del proyecto para Claude

> Este archivo lo lee Claude Code al abrir el proyecto. Resume TODO lo que
> se hizo y los errores que ya se resolvieron, para no repetirlos.

## Reglas para Claude
- **Responder SIEMPRE en español** (el usuario lo pidió muchas veces; incluso
  avisos cortos y mensajes tras tareas en segundo plano).
- El usuario es estudiante, no experto: explicar simple, paso a paso, con tablas.
- Antes de editar mucho, **confirmar que se entendió lo que quiere** (el usuario
  prefiere conversar primero cuando cambia la idea).
- Nunca escribir ni repetir la clave del WiFi (`secretos.h`, lo llena el usuario).
- Código muy comentado, en español, con sección de "errores comunes".
- Compilar SIEMPRE con arduino-cli antes de pedir que suba; avanzar por pasos.

## Qué es VigIA
Proyecto del curso **Taller de Robótica (UPN, Perú)**, grupo de 4 (asesor:
Cesar Augusto Reyes Gutierrez). Una **ESP32-CAM** en un soporte **pan-tilt**
(2 servos MG996R) que **recorre el lugar buscando rostros nuevos**, les toma
**1 foto** y los **ignora durante 5 minutos** mientras sigue buscando a otros.
Pasados 5 min, la misma persona puede volver a ser fotografiada.
NO sigue a las personas.
**Cambio del 14 oct (decisión del usuario: priorizar que funcione bien sobre la
privacidad)**: la decisión "¿nueva o conocida?" la toma la **PC con reconocimiento
facial** (ver "Reconocimiento en la PC"). El informe ya NO puede decir "no
identifica a nadie / sin biometría": hay que reescribir esa parte.

## Hardware y conexiones
| Parte | Detalle |
|---|---|
| ESP32-CAM | AI-Thinker, OV2640, PSRAM 4 MB. **Montada de cabeza** |
| Arduino Uno | Maneja los servos (pines 9 pan, 10 tilt). También sirve de **programador** del ESP32 |
| Servos | 2× MG996R, **alimentados directo del cargador** (empalmados, no por protoboard) |
| Energía | Cargador de celular 5V con cable USB cortado (rojo +, negro −) |
| microSD | **No funciona** (error 0x107 siempre: tarjeta o ranura). No depender de ella |
| HC-SR04 | Se quitó del proyecto |

### Modo USO (funcionando)
- ESP32 **IO13 → pin 2** del Uno (órdenes "P95 T88", 9600 baud, solo TX).
- ESP32 5V → **+ del cargador** (no del 5V del Uno: con WiFi da brownout).
- **Todos los GND unidos** (ESP32, Uno, cargador, servos).
- Uno: **sin** puente RESET→GND y **sin** cables en pines 0/1.

### Modo PROGRAMADOR (subir código al ESP32 con el Uno)
- Uno **RESET → GND**; Uno **TX(1) → U0T** y **RX(0) → U0R** (SIN cruzar).
- ESP32 **IO0 → GND**; reiniciar el ESP32 **desenchufando el cargador** (el
  USB del Uno no lo reinicia si el ESP32 va con el cargador).
- Subir: `arduino-cli upload -p COM5 --fqbn esp32:esp32:esp32cam --upload-property upload.speed=115200 <carpeta>`
- Para LEER el serial del ESP32 (diagnóstico): igual pero **sin IO0→GND**, a 115200.

### Herramientas en la PC
- arduino-cli: `C:\Users\stuar\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`
- Core ESP32 3.3.12 (FQBN `esp32:esp32:esp32cam`), Uno en COM5.
- Librería del modelo: `Documentos\Arduino\libraries\MUCKCITO-project-1_inferencing` (modelo 1.0.2).
- Compilar el ESP32 con la librería de IA tarda **~15 min** (siempre la recompila).
- En PowerShell usar `& $cli ... 2>&1 | Out-String` (`cmd /c` dentro de scripts largos fue bloqueado).
- **Segunda PC (usuario "Usuario", 14 oct)**: arduino-cli en
  `C:\Users\Usuario\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`
  (cores esp32 3.3.12 y avr OK). **Falta** la librería `MUCKCITO-project-1_inferencing`
  (no compila 05 ni 07 hasta copiarla). El proyecto está en el Escritorio (redirigido a
  OneDrive "Trabajos de comunicación 3"), **dentro de una carpeta doble**:
  `proyecto_Robotica-main\proyecto_Robotica-main\` (así se descomprimió el ZIP).
  El repo de GitHub (EduardoZA03/proyecto_Robotica) es privado.
- Python 3.13 con `opencv-python<5` (OpenCV 5 ya no trae el detector Haar), numpy, matplotlib.

## Programas (carpetas)
| Carpeta | Qué hace | Estado |
|---|---|---|
| 01_camara_sd_test | Prueba cámara + microSD | Cámara OK, SD falla |
| 02_servos_test | Servos directo en el ESP32 | Obsoleto (ahora los maneja el Uno) |
| **03_arduino_servos** | Uno: recibe "P.. T..", movimiento suave, modo demo (D) | **En uso** |
| 04_enlace_esp_arduino | Prueba del enlace ESP32→Uno | OK |
| 05_seguimiento_rostros | Seguimiento de rostros (sin fotos) | OK (versión estable) |
| 06_captura_dataset | Vista por WiFi + captura de fotos (**640x480 desde el 14 oct**, igual que el 07) | Compila; falta subirlo |
| **07_vigia_completo** | Sistema completo: buscar, fotografiar, ignorar, galería web | Ver "Estado actual" |
| dataset/ | Scripts de descarga (Open Images), captura y `analizar_firmas.py` | Imágenes NO van a GitHub |

## Valores calibrados (no cambiar sin probar)
- **Uno**: pan centro 110 (50–160), tilt centro 70 (10–120); VEL_MAX 50 °/s, ACEL 60 °/s².
- **ESP32**: mismos límites; `ARDUINO_VEL/ACEL` deben ser IGUALES a los del Uno.
- Imagen girada 180° en el sensor: `set_vflip(1)` + `set_hmirror(1)` (cámara de cabeza).
- `INVERTIR_PAN = true`, `INVERTIR_TILT = true` (probado el 14 oct, centra bien).
- GANANCIA 0.65, ZONA_MUERTA 4, PASO_MAX 15, UMBRAL 0.30, ASENTAR 120 ms.
- Ciclo "**mover → esperar → mirar**": esperar a que el brazo se detenga antes de
  capturar (fotos movidas = la IA no detecta).
- Latencia real de la IA: **~693 ms (QVGA) / ~719 ms (VGA)**. 150 ms NO es alcanzable
  con ESP32 clásico (solo con ESP32-S3); se recomendó ajustar el objetivo SMART.
- Modelo Edge Impulse 1.0.2 (FOMO MobileNetV2 0.35, 96×96 gris): F1 79.4%.

## Errores ya resueltos — NO repetir
1. **`crop_and_interpolate_rgb888` de Edge Impulse con buffer de salida distinto**
   copia primero el recorte completo (690 KB) en el destino → desborda la memoria
   (Guru Meditation / assert tlsf). Usar la versión in-place o `reducirAModelo()` propio.
2. **Cámara de cabeza**: sin vflip+hmirror la IA ve rostros al revés y detecta poco.
3. **Cables de pan y tilt estuvieron intercambiados**: si un eje se "escapa" a los
   extremos, revisar cables y luego el INVERTIR de ese eje.
4. **Cara fuera de alcance** (y≈4 con tilt en tope) NO es error de código: subir la
   cámara a la altura de la cara, persona a 1.5–3 m.
5. **Brownout** ("Brownout detector was triggered"): falta energía. Soluciones:
   capacitor 470–1000 µF en 5V/GND del ESP32, cargador separado para el ESP32,
   `WiFi.setTxPower(WIFI_POWER_8_5dBm)`, desactivar detector (último recurso).
6. **Monitor Serie abierto bloquea el puerto** (no se puede subir).
7. **IO0 puesto** → la placa espera código y no ejecuta nada (silencio total).
8. Línea serial con **solo ceros** = el ESP32 no tiene energía.
9. **Galería mostraba fotos viejas**: caché del navegador; se usa `idArranque` en la URL + `no-store`.
10. `camera_config_t config = {}` (sin inicializar daba errores raros); xclk 10 MHz.
11. **Argumentos por defecto** en funciones de un .ino → el preprocesador los duplica: no usarlos.
12. Mover archivos desde la carpeta OneDrive falla por permisos: **copiar y verificar**.
13. **Rutas largas** en Windows rompen la compilación (no usar build-path en carpetas profundas).
14. El usuario una vez pegó otro código encima de un .ino en Arduino IDE: pedirle que
    no edite los archivos ahí.

## Reconocimiento en la PC (14 oct) — `pc_servidor\servidor_vigia.py`
- Flujo: el 07 centra el rostro → `POST http://PC_IP:8000/rostro` con el JPEG 640x480 →
  la PC responde texto: `NUEVA <persona> <foto>` | `CONOCIDA <persona> <segundos>` | `SIN_ROSTRO`.
- PC: **YuNet** (rostro + 5 puntos: ojos, nariz, boca) → `alignCrop` → **SFace** (huella
  de 128 números) → similitud coseno, umbral **0.363**, hasta 6 vistas por persona.
  Ventana 5 min; pasada la ventana, otra foto con el MISMO número de persona.
- La PC guarda las fotos en `pc_servidor\fotos\` (+ `registro.csv`) y tiene galería en
  `http://<IP PC>:8000/`. `--probar a.jpg b.jpg` analiza fotos sin ESP32.
- Probado con fotos de OpenCV (Lena/Messi/mandril): misma persona 0.66–0.97, distinta
  0.19–0.28, mandril = SIN_ROSTRO. ~40–100 ms por foto en la PC.
- ESP32: `PC_IP` en secretos.h (si falta, `#define PC_IP ""` → modo antiguo). La firma
  de color se sigue guardando con el número de la PC para **ignorar de lejos**
  (UMBRAL_IGNORAR) sin preguntar. `SIN_ROSTRO` → se guarda como "persona" 0 (ignorar 5 min
  falsos positivos de FOMO). Si la PC falla → firma de color + flash (respaldo), y no
  vuelve a probar la PC por 30 s.
- **OpenCV no abre archivos con tildes en la ruta** ("comunicación"): los modelos se
  cargan desde memoria (`np.fromfile` + `create("onnx", buffer, ...)`); fotos con
  `imdecode`/`tofile`.
- **Firewall de Windows**: la 1.ª vez hay que permitir Python en redes privadas.
- Compila (14 oct) con una **librería de Edge Impulse simulada** en `C:\vigia_c06\libs`
  (solo para revisar errores; NO subir ese binario). Con la librería real hay que
  compilar de nuevo. En esta PC LittleFS compiló bien.

## Re-identificación de personas (límite conocido)
- Se usa una **firma de color de la cabeza** (no de la ropa: en el laboratorio todos
  usan **bata blanca**): 3 franjas (pelo/frente, ojos/nariz, boca/mentón) con
  cromaticidad 4×4 + brillo. 64 números. Se olvida a los 5 min.
- **Medido**: misma persona s91–95, persona distinta s80–89 → **los rangos se cruzan**;
  no distingue bien a personas de piel/pelo parecidos.
- Configurado en **opción A (estricto)**: `UMBRAL_IGNORAR 0.96`, `UMBRAL_MISMA 0.92`
  → no se pierde a nadie, pero se repiten fotos.
- **Opción B pendiente** (para el proyecto final): plantilla del rostro en grises con
  búsqueda de desplazamiento. Distingue mejor, pero se acerca a reconocimiento facial:
  habría que ajustar la frase "sin identificación biométrica" del informe.
- **Herramienta de medición (14 oct)**: `dataset\analizar_firmas.py`. Lee fotos de
  `dataset\muestra\personaN\*.jpg` (tomadas con el 06 + `captura_fotos.ps1 -Carpeta muestra\personaN`),
  detecta el rostro con Haar, calcula la firma **idéntica** a `calcularFirma()` (verificado
  contra una copia literal del C) y una variante **escalada al tamaño del rostro** (la actual
  usa franjas fijas en píxeles: de lejos mezcla fondo). Muestra % de fotos repetidas / personas
  perdidas por umbral, recomienda umbral, `--revisar` dibuja franjas, `--grafico` hace PNG.
  Decisión del 14 oct: **NO entrenar la IA en Python** (Edge Impulse da lo mismo); Python solo
  para medir y ajustar. Muestra: nombres genéricos, con permiso, borrar fotos al terminar.

## Estado actual (14 oct 2026)
- **Cargado en el ESP32**: versión 07 con fotos en PSRAM, WiFi a 8.5 dBm y opción A.
  Funciona (tomó fotos de 2 personas e ignoró a la ya registrada), PERO se
  reinicia cada 1–2 min por **brownout** y pierde las fotos.
- **Escrita pero NO compilada**: 07 con fotos en **flash (LittleFS)** (máx 40, no se
  pierden al reiniciar, numeración continua en Preferences) + **brownout desactivado**.
  Falló la compilación: "LittleFS.h: No such file" (existe en el core; probable caché de
  detección de librerías). Siguiente intento: `--build-path D:\terminal\_b7` (ruta
  corta) y subir con `--input-dir`; si sigue fallando, cambiar a `SPIFFS.h`.
- Galería web: `http://<IP del ESP32>/` (en casa fue 192.168.18.74). Las fotos más
  viejas se borran solas (el usuario no quiere botón de borrar por ahora).

## Pendientes
1. Compilar y probar la versión con fotos en flash.
2. Capacitor o cargador aparte para el ESP32 (brownout).
3. Subir las fotos a la nube (el usuario lo hará más adelante).
4. microSD: probar otra tarjeta ≤32 GB.
5. Opción B de re-identificación (decidir con el grupo por el tema de privacidad).
6. Informe: ajustar objetivo SMART de latencia (150 ms → valor medido ~700 ms) y
   mencionar ESP32-S3 como mejora futura.
9. **Probar el reconocimiento en la PC con el ESP32 real**: copiar la librería de
   Edge Impulse a esta PC (o compilar en la otra), poner `PC_IP` en secretos.h,
   compilar y subir el 07, correr `python servidor_vigia.py` y probar con el grupo.
   Ajustar `UMBRAL_COSENO` si confunde (subir) o repite (bajar).
10. Reescribir en el informe la parte de privacidad ("sin biometría").
7. (Menos prioritario ahora que hay reconocimiento en la PC) Subir el 06 (640x480), tomar la muestra (8–10 personas, 10–20 fotos c/u, en el
   laboratorio con bata) y correr `analizar_firmas.py`; con los números, ajustar
   `UMBRAL_MISMA`/`UMBRAL_IGNORAR` y decidir si se pasa a la firma escalada en el 07.
8. Revisar en Edge Impulse si el modelo se exportó **cuantizado (int8) + EON**; si no,
   podría bajar la latencia. Reentrenar con fotos de la ESP32-CAM para detectar mejor.
