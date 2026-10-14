/*
 * VigIA - Paso 1: Prueba de cámara + microSD
 * ------------------------------------------------------------
 * Objetivo: verificar que el hardware base funciona.
 *   1. Monta la microSD.
 *   2. Inicia la cámara OV2640.
 *   3. Toma UNA foto y la guarda como /foto_N.jpg
 *
 * Placa: AI-Thinker ESP32-CAM
 * Arduino IDE -> Herramientas:
 *   Placa:            "AI Thinker ESP32-CAM"
 *   Velocidad serial: 115200 (en el Monitor Serie)
 *
 * Cableado FTDI (poner el FTDI en 5V):
 *   FTDI 5V  -> ESP32-CAM 5V
 *   FTDI GND -> ESP32-CAM GND
 *   FTDI TX  -> ESP32-CAM U0R (GPIO3)
 *   FTDI RX  -> ESP32-CAM U0T (GPIO1)
 *
 * Para SUBIR el código:
 *   1. Puentear GPIO0 con GND.
 *   2. Reiniciar (desconectar y reconectar la alimentación).
 *   3. Click en "Subir".
 * Para EJECUTAR:
 *   1. Quitar el puente GPIO0-GND.
 *   2. Abrir el Monitor Serie (115200) y reiniciar.
 *
 * Errores comunes:
 *   "Brownout detector was triggered" -> alimentación insuficiente.
 *       Usar 5V (no 3.3V), cables cortos, o fuente externa 5V 2A.
 *   "Camera init failed 0x105/0x20004" -> cámara mal conectada.
 *       Revisar que el flex de la cámara esté bien insertado y trabado.
 *   "I2C hardware timeout" / "probe device timeout" -> NORMAL si después
 *       aparece "Camara OK": el driver prueba varios modelos de cámara.
 *   "Card Mount Failed" / "0x107" -> microSD no responde.
 *       Reinsertar hasta el clic, limpiar contactos, nada conectado en
 *       IO2/IO14/IO15, formatear en FAT32, probar otra tarjeta (≤32GB).
 *   "Failed to connect to ESP32" al subir ->
 *       no está en modo programación: revisar puente GPIO0-GND y reiniciar.
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <Preferences.h>

// ---- Pines de la cámara (modelo AI-Thinker) ----
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

Preferences prefs;  // guarda el número de foto aunque se apague

bool iniciarCamara() {
  camera_config_t config = {};  // todo en cero: evita valores basura
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  // 10 MHz: más estable que 20 MHz con PSRAM (evita "FB-OVF" continuo)
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  // Siempre entregar el cuadro más reciente
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  // Si la placa tiene PSRAM (la AI-Thinker sí) usamos más resolución
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_SVGA;  // 800x600
    config.jpeg_quality = 12;              // 0-63, menor = mejor calidad
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_VGA;   // 640x480
    config.jpeg_quality = 15;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("ERROR: Camera init failed 0x%x\n", err);
    return false;
  }
  return true;
}

bool iniciarSD() {
  // Modo 1-bit: usa menos pines (2, 14, 15) y deja libres
  // GPIO 12 y 13 para los servos en el Paso 2.
  // También evita que el LED flash (GPIO4) parpadee al escribir.
  // Reintentos: primero a velocidad normal (20 MHz) y, si falla,
  // a velocidad lenta (400 kHz), que tolera peores contactos.
  bool montada = false;
  for (int intento = 1; intento <= 3 && !montada; intento++) {
    int freq = (intento < 3) ? SDMMC_FREQ_DEFAULT : SDMMC_FREQ_PROBING;
    montada = SD_MMC.begin("/sdcard", true, false, freq);
    Serial.printf("microSD intento %d (%d kHz): %s\n", intento, freq, montada ? "OK" : "fallo");
    if (!montada) {
      SD_MMC.end();
      delay(500);
    }
  }
  if (!montada) {
    Serial.println("ERROR: Card Mount Failed");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("ERROR: no hay microSD insertada");
    return false;
  }
  Serial.printf("microSD OK: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== VigIA - Paso 1: camara + SD ===");
  Serial.printf("PSRAM: %s\n", psramFound() ? "SI" : "NO");

  // La microSD va PRIMERO: así arranca antes de que la cámara empiece
  // a consumir corriente (evita el error 0x107 con alimentación justa)
  // Si la microSD falla, igual se prueba la cámara (la foto no se guarda)
  bool hayTarjeta = iniciarSD();
  if (!hayTarjeta) Serial.println("AVISO: sin microSD, solo se prueba la camara");

  if (!iniciarCamara()) return;
  Serial.println("Camara OK");

  // Descartar los primeros cuadros: salen oscuros mientras
  // la cámara ajusta exposición y balance de blancos
  for (int i = 0; i < 5; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    Serial.printf("Cuadro de prueba %d: %s\n", i + 1, fb ? "OK" : "FALLO");
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }

  Serial.println("Capturando foto...");
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("ERROR: no se pudo capturar la foto");
    return;
  }

  if (!hayTarjeta) {
    Serial.printf("OK: foto capturada en memoria (%u bytes, %ux%u), no se guarda\n",
                  fb->len, fb->width, fb->height);
    esp_camera_fb_return(fb);
    Serial.println("Listo. Reinicia para tomar otra foto.");
    return;
  }

  prefs.begin("vigia", false);
  uint32_t n = prefs.getUInt("foto", 0) + 1;

  String ruta = "/foto_" + String(n) + ".jpg";
  File archivo = SD_MMC.open(ruta, FILE_WRITE);
  if (!archivo) {
    Serial.println("ERROR: no se pudo crear el archivo en la SD");
  } else {
    archivo.write(fb->buf, fb->len);
    archivo.close();
    prefs.putUInt("foto", n);
    Serial.printf("OK: foto guardada en %s (%u bytes)\n", ruta.c_str(), fb->len);
  }

  prefs.end();
  esp_camera_fb_return(fb);
  Serial.println("Listo. Reinicia para tomar otra foto.");
}

void loop() {
  // Nada: el Paso 1 solo toma una foto al encender
}
