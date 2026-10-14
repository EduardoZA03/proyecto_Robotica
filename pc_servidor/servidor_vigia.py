"""
VigIA - Servidor de reconocimiento facial en la PC
--------------------------------------------------------------------------
El ESP32 sigue haciendo lo de siempre (recorrer, detectar con FOMO, centrar
y tomar la foto), pero la decisión "¿es una persona NUEVA o ya la vi?" la
toma la PC, que es mucho más potente:

  ESP32 --(foto JPEG por WiFi)--> PC
        1. YuNet ubica el rostro y 5 puntos: 2 ojos, nariz y 2 comisuras de la boca.
        2. Con esos puntos se endereza la cara (alignCrop).
        3. SFace la convierte en 128 números (su "huella").
        4. Se compara con las personas ya vistas (similitud coseno).
  PC  --(respuesta de texto)----> ESP32
        "NUEVA <persona> <foto>"    -> persona nueva: la PC guardó la foto
        "CONOCIDA <persona> <seg>"  -> ya fotografiada hace <seg> segundos
        "SIN_ROSTRO"                -> FOMO se equivocó (no era una cara)

Igual que antes: una foto por persona cada 5 minutos. Pasados los 5 minutos,
si la misma persona aparece de nuevo se le toma otra foto (con el MISMO
número de persona, porque la PC sí la reconoce).

Las fotos se guardan en pc_servidor\\fotos\\ y se ven en la galería:
  http://<IP de la PC>:8000/

Uso (PowerShell, dentro de la carpeta pc_servidor):
  pip install "opencv-python<5" numpy        (solo la primera vez)
  python servidor_vigia.py                   -> inicia el servidor
  python servidor_vigia.py --probar a.jpg b.jpg c.jpg
        -> prueba SIN ESP32: analiza las fotos en orden, como si llegaran
           de la cámara, y dice qué respondería (no guarda nada)

Al arrancar muestra la IP de la PC: copiarla en secretos.h del 07:
  #define PC_IP "192.168.18.50"

Los modelos (YuNet y SFace, de OpenCV) se descargan solos la primera vez en
pc_servidor\\modelos\\ (~39 MB).

Errores comunes:
  El ESP32 dice "PC no responde" -> revisar que el servidor esté corriendo,
      que PC y ESP32 estén en el MISMO WiFi y que la IP en secretos.h sea la
      que muestra el servidor. La 1.a vez Windows pregunta por el FIREWALL:
      marcar "Redes privadas" y Permitir. Si se negó: Panel de control >
      Firewall > Permitir una aplicación > Python > marcar Privada.
  "No module named cv2" -> pip install "opencv-python<5" numpy
  "FaceDetectorYN" no existe -> OpenCV muy viejo: pip install -U "opencv-python<5"
  Confunde a dos personas -> subir UMBRAL_COSENO (0.40). Si a la misma persona
      la toma como nueva muy seguido -> bajarlo (0.32).
  La IP de la PC cambió (otro día / otra red) -> actualizar secretos.h y subir el 07.
"""

import argparse
import csv
import datetime
import socket
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

import numpy as np

try:
    import cv2
except ImportError:
    sys.exit('Falta OpenCV. Instalar con:  pip install "opencv-python<5" numpy')

# ---- Ajustes ----
PUERTO = 8000
VENTANA_S = 5 * 60          # 5 minutos por persona (igual que el 07)
UMBRAL_COSENO = 0.363       # similitud mínima "misma persona" (valor recomendado por OpenCV para SFace)
CONF_DETECCION = 0.6        # confianza mínima de YuNet para aceptar un rostro
MAX_VISTAS = 6              # "huellas" guardadas por persona (distintos ángulos y luces)

CARPETA = Path(__file__).resolve().parent
MODELOS = CARPETA / "modelos"
FOTOS = CARPETA / "fotos"
REGISTRO = FOTOS / "registro.csv"
URL_ZOO = "https://github.com/opencv/opencv_zoo/raw/main/models/"
ARCHIVOS_MODELO = {
    "yunet": ("face_detection_yunet_2023mar.onnx", URL_ZOO + "face_detection_yunet/face_detection_yunet_2023mar.onnx"),
    "sface": ("face_recognition_sface_2021dec.onnx", URL_ZOO + "face_recognition_sface/face_recognition_sface_2021dec.onnx"),
}


def ruta_modelo(clave):
    """Devuelve la ruta del modelo y lo descarga si todavía no está."""
    nombre, url = ARCHIVOS_MODELO[clave]
    ruta = MODELOS / nombre
    if not ruta.exists():
        MODELOS.mkdir(parents=True, exist_ok=True)
        print(f"Descargando {nombre} (solo la primera vez)...")
        urllib.request.urlretrieve(url, ruta)
    return ruta


def ahora_texto():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# ===========================================================================
#  Reconocimiento
# ===========================================================================
class Reconocedor:
    def __init__(self, guardar=True):
        # Los modelos se cargan desde la memoria y no desde la ruta: OpenCV no
        # abre archivos si la ruta tiene tildes ("comunicación").
        vacio = np.array([], np.uint8)
        yunet = np.fromfile(str(ruta_modelo("yunet")), np.uint8)
        sface = np.fromfile(str(ruta_modelo("sface")), np.uint8)
        self.detector = cv2.FaceDetectorYN.create("onnx", yunet, vacio, (320, 320),
                                                  CONF_DETECCION, 0.3, 5000)
        self.sface = cv2.FaceRecognizerSF.create("onnx", sface, vacio)
        self.personas = []        # {"id", "vistas": [huella...], "ultima_foto": time}
        self.siguiente_id = 1
        self.guardar = guardar
        self.candado = threading.Lock()   # OpenCV no es seguro con 2 hilos a la vez
        self.siguiente_foto = 1
        if guardar:
            FOTOS.mkdir(exist_ok=True)
            # La numeración de fotos sigue después de reiniciar el servidor
            numeros = [int(p.name.split("_")[0]) for p in FOTOS.glob("*.jpg") if p.name.split("_")[0].isdigit()]
            self.siguiente_foto = max(numeros, default=0) + 1

    def _elegir_rostro(self, caras, ancho, alto):
        """El ESP32 ya centró el rostro: se elige el más cercano al centro."""
        cx, cy = ancho / 2, alto / 2
        return min(caras, key=lambda c: (c[0] + c[2] / 2 - cx) ** 2 + (c[1] + c[3] / 2 - cy) ** 2)

    def _buscar(self, huella):
        """Persona más parecida y su similitud (la mejor entre todas sus vistas)."""
        mejor, mejor_sim = None, -1.0
        for p in self.personas:
            for v in p["vistas"]:
                s = self.sface.match(huella, v, cv2.FaceRecognizerSF_FR_COSINE)
                if s > mejor_sim:
                    mejor, mejor_sim = p, s
        return mejor, mejor_sim

    def analizar(self, jpg):
        """Recibe los bytes de un JPEG y devuelve un dict con la decisión."""
        with self.candado:
            img = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
            if img is None:
                return {"resultado": "ERROR", "detalle": "la imagen no es un JPEG válido"}
            alto, ancho = img.shape[:2]
            self.detector.setInputSize((ancho, alto))
            _, caras = self.detector.detect(img)
            if caras is None or len(caras) == 0:
                return {"resultado": "SIN_ROSTRO"}

            cara = self._elegir_rostro(caras, ancho, alto)
            alineada = self.sface.alignCrop(img, cara)        # usa ojos, nariz y boca
            huella = self.sface.feature(alineada).copy()
            persona, sim = self._buscar(huella)
            ahora = time.time()

            if persona is not None and sim >= UMBRAL_COSENO:
                # Guardar esta vista (otro ángulo/luz) para reconocerla mejor después
                persona["vistas"] = (persona["vistas"] + [huella])[-MAX_VISTAS:]
                hace = ahora - persona["ultima_foto"]
                if hace < VENTANA_S:
                    return {"resultado": "CONOCIDA", "persona": persona["id"],
                            "hace": int(hace), "similitud": sim}
                # Pasaron los 5 minutos: otra foto de la misma persona
            else:
                persona = {"id": self.siguiente_id, "vistas": [huella], "ultima_foto": 0}
                self.siguiente_id += 1
                self.personas.append(persona)

            persona["ultima_foto"] = ahora
            n = self._guardar_foto(jpg, persona["id"])
            return {"resultado": "NUEVA", "persona": persona["id"], "foto": n,
                    "similitud": sim if sim > -1 else None}

    def _guardar_foto(self, jpg, id_persona):
        n = self.siguiente_foto
        self.siguiente_foto += 1
        if not self.guardar:
            return n
        hora = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        (FOTOS / f"{n:04d}_p{id_persona}_{hora}.jpg").write_bytes(jpg)
        return n


def anotar_registro(r):
    """Agrega una línea a fotos\\registro.csv (sirve para el informe)."""
    nuevo = not REGISTRO.exists()
    with open(REGISTRO, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if nuevo:
            w.writerow(["hora", "resultado", "persona", "foto", "similitud"])
        sim = r.get("similitud")
        w.writerow([ahora_texto(), r["resultado"], r.get("persona", ""), r.get("foto", ""),
                    "" if sim is None else f"{sim:.3f}"])


def texto_respuesta(r):
    """Respuesta corta para el ESP32 (fácil de leer con sscanf)."""
    if r["resultado"] == "NUEVA":
        return f"NUEVA {r['persona']} {r['foto']}"
    if r["resultado"] == "CONOCIDA":
        return f"CONOCIDA {r['persona']} {r['hace']}"
    return r["resultado"]


def texto_consola(r):
    sim = r.get("similitud")
    s = "" if sim is None else f" (similitud {sim:.2f})"
    if r["resultado"] == "NUEVA":
        return f"FOTO #{r['foto']} persona {r['persona']}{s}"
    if r["resultado"] == "CONOCIDA":
        return f"persona {r['persona']} ya registrada hace {r['hace']} s{s}"
    if r["resultado"] == "SIN_ROSTRO":
        return "no hay rostro (FOMO se equivocó)"
    return f"ERROR: {r.get('detalle', '')}"


# ===========================================================================
#  Servidor web
# ===========================================================================
reconocedor = None


class Manejador(BaseHTTPRequestHandler):
    def log_message(self, *args):   # sin el registro por defecto (muy largo)
        pass

    def _responder(self, codigo, tipo, datos):
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(datos)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(datos)

    def do_POST(self):
        if urlparse(self.path).path != "/rostro":
            return self._responder(404, "text/plain", b"no existe")
        largo = int(self.headers.get("Content-Length", 0))
        if largo <= 0 or largo > 2_000_000:
            return self._responder(400, "text/plain", b"ERROR tamano")
        jpg = self.rfile.read(largo)
        t0 = time.time()
        r = reconocedor.analizar(jpg)
        ms = int((time.time() - t0) * 1000)
        print(f"[{ahora_texto()}] {self.client_address[0]}: {texto_consola(r)}  [{ms} ms]")
        if reconocedor.guardar and r["resultado"] in ("NUEVA", "CONOCIDA"):
            anotar_registro(r)
        self._responder(200, "text/plain", texto_respuesta(r).encode())

    def do_GET(self):
        ruta = urlparse(self.path).path
        if ruta == "/estado":
            return self._responder(200, "text/plain", b"OK")
        if ruta.startswith("/foto/"):
            nombre = unquote(ruta[len("/foto/"):])
            archivo = FOTOS / nombre
            # Solo archivos .jpg que estén DENTRO de la carpeta fotos
            if Path(nombre).name != nombre or not nombre.endswith(".jpg") or not archivo.is_file():
                return self._responder(404, "text/plain", b"foto no disponible")
            return self._responder(200, "image/jpeg", archivo.read_bytes())
        if ruta == "/":
            return self._responder(200, "text/html; charset=utf-8", galeria().encode())
        self._responder(404, "text/plain", b"no existe")


def galeria():
    fotos = sorted(FOTOS.glob("*.jpg"), reverse=True)[:80]
    figuras = []
    for f in fotos:
        partes = f.stem.split("_")   # 0001_p3_20261014_153000
        try:
            hora = datetime.datetime.strptime(partes[2] + partes[3], "%Y%m%d%H%M%S").strftime("%d/%m %H:%M:%S")
            texto = f"#{int(partes[0])} - persona {partes[1][1:]}<br>{hora}"
        except (IndexError, ValueError):
            texto = f.name
        figuras.append(f"<figure><a href='/foto/{f.name}'><img src='/foto/{f.name}'></a>"
                       f"<figcaption>{texto}</figcaption></figure>")
    return ("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width'>"
            "<meta http-equiv='refresh' content='10'><title>VigIA - registro</title>"
            "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:12px}"
            ".g{display:flex;flex-wrap:wrap;gap:10px}figure{margin:0;background:#222;padding:6px;border-radius:8px}"
            "img{width:240px;display:block;border-radius:4px}figcaption{font-size:13px;margin-top:4px}</style>"
            "</head><body><h2>VigIA - registro de personas (PC)</h2>"
            f"<p>Fotos guardadas: {len(list(FOTOS.glob('*.jpg')))}. Se actualiza cada 10 s.</p>"
            f"<div class='g'>{''.join(figuras)}</div></body></html>")


def ip_local():
    """IP de la PC en la red WiFi (no envía nada a internet)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def probar(archivos):
    """Analiza fotos de la PC en orden, como si llegaran del ESP32 (no guarda nada)."""
    rec = Reconocedor(guardar=False)
    for nombre in archivos:
        ruta = Path(nombre)
        if not ruta.is_file():
            print(f"{nombre}: no existe")
            continue
        r = rec.analizar(ruta.read_bytes())
        print(f"{ruta.name}: {texto_respuesta(r):<16} | {texto_consola(r)}")


def main():
    global reconocedor
    parser = argparse.ArgumentParser(description="Servidor de reconocimiento facial de VigIA.")
    parser.add_argument("--probar", nargs="+", metavar="FOTO", help="analizar fotos sin ESP32")
    parser.add_argument("--puerto", type=int, default=PUERTO)
    args = parser.parse_args()

    if args.probar:
        return probar(args.probar)

    reconocedor = Reconocedor(guardar=True)
    ip = ip_local()
    print("=== VigIA - servidor de reconocimiento (PC) ===")
    print(f"Galería:          http://{ip}:{args.puerto}/")
    print(f"En secretos.h:    #define PC_IP \"{ip}\"")
    print(f"Fotos en:         {FOTOS}")
    print("Esperando fotos del ESP32... (Ctrl+C para salir)\n")
    servidor = ThreadingHTTPServer(("0.0.0.0", args.puerto), Manejador)
    try:
        servidor.serve_forever()
    except KeyboardInterrupt:
        print("\nServidor detenido.")


if __name__ == "__main__":
    main()
