/*
 * VigIA - Paso 6: Captura de fotos para reentrenar el modelo
 * ------------------------------------------------------------
 * La ESP32-CAM se conecta al WiFi y publica su imagen:
 *   http://<IP>/       -> página con la imagen en vivo
 *   http://<IP>/foto   -> una foto JPEG 640x480 (la misma resolución
 *                         que usa 07_vigia_completo)
 * El script captura_fotos.ps1 (en la carpeta dataset) descarga las
 * fotos automáticamente a la PC. No hace falta la microSD.
 *
 * (14 oct) Se cambió de 320x240 a 640x480 para que las fotos sean iguales
 * a las del 07. Así sirven para dataset\analizar_firmas.py (mide la firma
 * de color con la misma imagen que ve el ESP32) y para Edge Impulse.
 *
 * ANTES DE SUBIR: escribir el nombre y la clave del WiFi en la pestaña
 * "secretos.h" (Arduino IDE la muestra como pestaña junto a este archivo)
 * y guardar con Ctrl+S.
 *
 * La IP aparece en el Monitor Serie (115200) al arrancar.
 * Importante: la PC debe estar en la MISMA red WiFi.
 *
 * Errores comunes:
 *   "WiFi: no se pudo conectar" -> revisar nombre/clave en secretos.h.
 *       La ESP32 solo funciona con redes de 2.4 GHz (no 5 GHz).
 *   "Brownout" al conectar el WiFi -> el WiFi consume más corriente:
 *       alimentar el ESP32 con el cargador 5V en vez del Arduino/FTDI.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include "secretos.h"

// ---- Pines de la cámara (AI-Thinker) ----
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

WebServer servidor(80);
uint32_t fotosServidas = 0;

bool iniciarCamara() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;     // 640x480, igual que 07_vigia_completo
  config.jpeg_quality = 10;                // algo mejor calidad para el dataset
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&config) != ESP_OK) return false;

  // La cámara está montada de cabeza: girar la imagen 180°
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
  return true;
}

void paginaPrincipal() {
  servidor.send(200, "text/html",
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width'>"
    "<title>VigIA - camara</title></head>"
    "<body style='font-family:sans-serif;text-align:center;background:#111;color:#eee'>"
    "<h3>VigIA - vista de la camara</h3>"
    "<img id='f' src='/foto' style='width:90%;max-width:640px'>"
    "<p>Se actualiza cada segundo. Las fotos las guarda el script de la PC.</p>"
    "<script>setInterval(()=>{document.getElementById('f').src='/foto?t='+Date.now()},1000)</script>"
    "</body></html>");
}

void enviarFoto() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    servidor.send(503, "text/plain", "error de captura");
    return;
  }
  servidor.sendHeader("Cache-Control", "no-store");
  servidor.setContentLength(fb->len);
  servidor.send(200, "image/jpeg", "");
  servidor.sendContent((const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  fotosServidas++;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== VigIA - Paso 6: captura de fotos por WiFi ===");

  if (!iniciarCamara()) {
    Serial.println("ERROR: camara");
    while (true) delay(1000);
  }
  Serial.println("Camara OK");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  Serial.printf("Conectando a \"%s\"", WIFI_NOMBRE);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: no se pudo conectar (revisar secretos.h y que sea 2.4 GHz)");
    while (true) delay(1000);
  }

  servidor.on("/", paginaPrincipal);
  servidor.on("/foto", enviarFoto);
  servidor.begin();

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("Abrir en el navegador: http://%s/\n", WiFi.localIP().toString().c_str());
}

void loop() {
  servidor.handleClient();
}
