# VigIA - Servidor de reconocimiento facial en la PC.
# Uso: python servidor_vigia.py   |   python servidor_vigia.py --probar a.jpg b.jpg

import argparse
import csv
import datetime
import json
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

PUERTO = 8000                 # puerto del servidor web
VENTANA_S = 5 * 60            # segundos que se ignora a una persona ya fotografiada
UMBRAL_COSENO = 0.363         # similitud mínima para decir "misma persona"
CONF_DETECCION = 0.6          # confianza mínima de YuNet para aceptar un rostro
MAX_VISTAS = 6                # huellas guardadas por persona
CENTRO_PX = 70                # distancia al centro (px) para fotografiar al escanear

CALIDAD_MIN_ANCHO = 40        # ancho mínimo del rostro (px)
CALIDAD_MIN_PUNTAJE = 0.75    # confianza mínima de YuNet para registrar
CALIDAD_MIN_NITIDEZ = 25.0    # nitidez mínima (varianza del Laplaciano)
CALIDAD_MAX_GIRO = 0.35       # giro máximo de la cara (0 = de frente)
MAX_REPETIR = 3               # intentos antes de aceptar una foto de baja calidad

PERSONAS_ARCHIVO = "personas.json"   # archivo con las huellas de las personas

CARPETA = Path(__file__).resolve().parent   # carpeta de este programa
MODELOS = CARPETA / "modelos"               # carpeta de los modelos YuNet y SFace
FOTOS = CARPETA / "fotos"                   # carpeta de las fotos
REGISTRO = FOTOS / "registro.csv"           # registro de fotos (para el informe)
DESCARTADAS = CARPETA / "descartadas"       # imágenes donde no se encontró rostro
URL_ZOO = "https://github.com/opencv/opencv_zoo/raw/main/models/"   # origen de los modelos
ARCHIVOS_MODELO = {                         # nombre de archivo y URL de cada modelo
    "yunet": ("face_detection_yunet_2023mar.onnx", URL_ZOO + "face_detection_yunet/face_detection_yunet_2023mar.onnx"),
    "sface": ("face_recognition_sface_2021dec.onnx", URL_ZOO + "face_recognition_sface/face_recognition_sface_2021dec.onnx"),
}


# Devuelve la ruta de un modelo y lo descarga si todavía no está
def ruta_modelo(clave):
    nombre, url = ARCHIVOS_MODELO[clave]
    ruta = MODELOS / nombre
    if not ruta.exists():
        MODELOS.mkdir(parents=True, exist_ok=True)
        print(f"Descargando {nombre} (solo la primera vez)...")
        urllib.request.urlretrieve(url, ruta)
    return ruta


# Devuelve la fecha y hora actual como texto
def ahora_texto():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


_clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))   # ecualizador de brillo


# Mejora el contraste del brillo (CLAHE) sin cambiar los colores
def mejorar_luz(img):
    lab = cv2.cvtColor(img, cv2.COLOR_BGR2LAB)
    l, a, b = cv2.split(lab)
    return cv2.cvtColor(cv2.merge((_clahe.apply(l), a, b)), cv2.COLOR_LAB2BGR)


# Detecta rostros, calcula su huella y decide si la persona es nueva o conocida
class Reconocedor:
    # Carga los modelos y las personas guardadas
    def __init__(self, guardar=True):
        vacio = np.array([], np.uint8)
        yunet = np.fromfile(str(ruta_modelo("yunet")), np.uint8)
        sface = np.fromfile(str(ruta_modelo("sface")), np.uint8)
        self.detector = cv2.FaceDetectorYN.create("onnx", yunet, vacio, (320, 320),
                                                  CONF_DETECCION, 0.3, 5000)   # YuNet: encuentra rostros
        self.sface = cv2.FaceRecognizerSF.create("onnx", sface, vacio)        # SFace: calcula huellas
        self.personas = []                  # personas registradas (id, última foto, huellas)
        self.siguiente_id = 1               # número para la próxima persona nueva
        self.guardar = guardar              # False = modo prueba, no guarda nada
        self.candado = threading.Lock()     # evita analizar dos fotos a la vez
        self.siguiente_foto = 1             # número para la próxima foto
        self.repetidas = 0                  # fotos de baja calidad seguidas pedidas de nuevo
        if guardar:
            FOTOS.mkdir(exist_ok=True)
            numeros = [int(p.name.split("_")[0]) for p in FOTOS.glob("*.jpg") if p.name.split("_")[0].isdigit()]
            self.siguiente_foto = max(numeros, default=0) + 1
            self._cargar_personas()

    # Lee las personas guardadas en personas.json
    def _cargar_personas(self):
        ruta = FOTOS / PERSONAS_ARCHIVO
        if not ruta.exists():
            return
        try:
            datos = json.loads(ruta.read_text(encoding="utf-8"))
            self.siguiente_id = datos["siguiente_id"]
            self.personas = [{"id": p["id"], "ultima_foto": p["ultima_foto"],
                              "vistas": [np.array(v, np.float32).reshape(1, -1) for v in p["vistas"]]}
                             for p in datos["personas"]]
            print(f"Recordando {len(self.personas)} persona(s) registradas antes.")
        except (OSError, ValueError, KeyError) as e:
            print(f"No se pudo leer {PERSONAS_ARCHIVO} ({e}): se empieza de cero.")

    # Guarda las personas en personas.json
    def _guardar_personas(self):
        if not self.guardar:
            return
        datos = {"siguiente_id": self.siguiente_id,
                 "personas": [{"id": p["id"], "ultima_foto": p["ultima_foto"],
                               "vistas": [v.flatten().tolist() for v in p["vistas"]]}
                              for p in self.personas]}
        temporal = FOTOS / (PERSONAS_ARCHIVO + ".tmp")
        temporal.write_text(json.dumps(datos), encoding="utf-8")
        temporal.replace(FOTOS / PERSONAS_ARCHIVO)

    # Revisa si el rostro sirve (tamaño, confianza, giro y nitidez); devuelve (ok, motivo)
    def _calidad(self, img, cara):
        ancho, puntaje = cara[2], cara[-1]
        ojo_der_x, ojo_izq_x, nariz_x = cara[4], cara[6], cara[8]
        if ancho < CALIDAD_MIN_ANCHO:
            return False, f"muy lejos ({ancho:.0f} px)"
        if puntaje < CALIDAD_MIN_PUNTAJE:
            return False, f"dudoso (YuNet {puntaje:.2f})"
        giro = abs(nariz_x - (ojo_der_x + ojo_izq_x) / 2) / max(1.0, abs(ojo_izq_x - ojo_der_x))
        if giro > CALIDAD_MAX_GIRO:
            return False, f"de lado (giro {giro:.2f})"
        gris = cv2.cvtColor(self.sface.alignCrop(img, cara), cv2.COLOR_BGR2GRAY)
        nitidez = cv2.Laplacian(gris, cv2.CV_64F).var()
        if nitidez < CALIDAD_MIN_NITIDEZ:
            return False, f"borroso (nitidez {nitidez:.0f})"
        return True, ""

    # Elige el rostro más cercano al centro de la imagen
    def _elegir_rostro(self, caras, ancho, alto):
        cx, cy = ancho / 2, alto / 2
        return min(caras, key=lambda c: (c[0] + c[2] / 2 - cx) ** 2 + (c[1] + c[3] / 2 - cy) ** 2)

    # Busca la persona más parecida a una huella; devuelve (persona, similitud)
    def _buscar(self, huella):
        mejor, mejor_sim = None, -1.0
        for p in self.personas:
            for v in p["vistas"]:
                s = self.sface.match(huella, v, cv2.FaceRecognizerSF_FR_COSINE)
                if s > mejor_sim:
                    mejor, mejor_sim = p, s
        return mejor, mejor_sim

    # Analiza la foto del rostro centrado por el ESP32 (ruta /rostro)
    def analizar(self, jpg):
        with self.candado:
            img = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
            if img is None:
                return {"resultado": "ERROR", "detalle": "la imagen no es un JPEG válido"}
            img = mejorar_luz(img)
            alto, ancho = img.shape[:2]
            self.detector.setInputSize((ancho, alto))
            _, caras = self.detector.detect(img)
            if caras is None or len(caras) == 0:
                self._guardar_descartada(img, jpg)
                return {"resultado": "SIN_ROSTRO", "detalle": self._puntaje_bajo(img)}

            cara = self._elegir_rostro(caras, ancho, alto)
            return self._registrar(img, jpg, cara)

    # Dice el mejor puntaje de YuNet con umbral bajo (para saber por qué no detectó)
    def _puntaje_bajo(self, img):
        self.detector.setScoreThreshold(0.1)
        _, caras = self.detector.detect(img)
        self.detector.setScoreThreshold(CONF_DETECCION)
        if caras is None or len(caras) == 0:
            return "YuNet no ve nada"
        return f"mejor puntaje YuNet {max(c[-1] for c in caras):.2f} (umbral {CONF_DETECCION})"

    # Guarda las imágenes sin rostro en "descartadas" (solo las últimas 30)
    def _guardar_descartada(self, img, jpg):
        if not self.guardar:
            return
        DESCARTADAS.mkdir(exist_ok=True)
        hora = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        (DESCARTADAS / f"sin_rostro_{hora}.jpg").write_bytes(jpg)
        for viejo in sorted(DESCARTADAS.glob("*.jpg"))[:-30]:
            viejo.unlink()

    # Endereza el rostro y calcula su huella de 128 números (SFace)
    def _huella(self, img, cara):
        alineada = self.sface.alignCrop(img, cara)
        return self.sface.feature(alineada).copy()

    # Busca rostros sin registrar en toda la imagen (ruta /escanear): NUEVA, IR, CONOCIDAS o NADA
    def escanear(self, jpg):
        with self.candado:
            img = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
            if img is None:
                return {"resultado": "ERROR", "detalle": "la imagen no es un JPEG válido"}
            img = mejorar_luz(img)
            alto, ancho = img.shape[:2]
            self.detector.setInputSize((ancho, alto))
            _, caras = self.detector.detect(img)
            if caras is None or len(caras) == 0:
                return {"resultado": "NADA"}
            ahora = time.time()
            candidatas = []
            for cara in caras:
                persona, sim = self._buscar(self._huella(img, cara))
                reciente = (persona is not None and sim >= UMBRAL_COSENO
                            and ahora - persona["ultima_foto"] < VENTANA_S)
                if not reciente:
                    candidatas.append(cara)
            if not candidatas:
                return {"resultado": "CONOCIDAS", "rostros": len(caras)}
            cara = self._elegir_rostro(candidatas, ancho, alto)
            dx = cara[0] + cara[2] / 2 - ancho / 2
            dy = cara[1] + cara[3] / 2 - alto / 2
            if abs(dx) <= CENTRO_PX and abs(dy) <= CENTRO_PX:
                r = self._registrar(img, jpg, cara)
                if r["resultado"] != "REPETIR":
                    return r
                return {"resultado": "IR", "dx": int(dx), "dy": int(dy), "rostros": len(caras),
                        "detalle": r["detalle"]}
            return {"resultado": "IR", "dx": int(dx), "dy": int(dy), "rostros": len(caras)}

    # Decide si el rostro es nuevo (guarda la foto), conocido o hay que repetir la foto
    def _registrar(self, img, jpg, cara):
        if True:
            huella = self._huella(img, cara)
            persona, sim = self._buscar(huella)
            ahora = time.time()

            buena, motivo = self._calidad(img, cara)

            if persona is not None and sim >= UMBRAL_COSENO:
                self.repetidas = 0
                if buena:
                    persona["vistas"] = (persona["vistas"] + [huella])[-MAX_VISTAS:]
                    self._guardar_personas()
                hace = ahora - persona["ultima_foto"]
                if hace < VENTANA_S:
                    return {"resultado": "CONOCIDA", "persona": persona["id"],
                            "hace": int(hace), "similitud": sim}

            else:
                if not buena and self.repetidas < MAX_REPETIR - 1:
                    self.repetidas += 1
                    return {"resultado": "REPETIR", "detalle": motivo, "similitud": sim if sim > -1 else None}
                self.repetidas = 0
                persona = {"id": self.siguiente_id, "vistas": [huella], "ultima_foto": 0}
                self.siguiente_id += 1
                self.personas.append(persona)

            persona["ultima_foto"] = ahora
            self._guardar_personas()
            n = self._guardar_foto(jpg, persona["id"])
            return {"resultado": "NUEVA", "persona": persona["id"], "foto": n,
                    "similitud": sim if sim > -1 else None}

    # Guarda la foto en la carpeta fotos y devuelve su número
    def _guardar_foto(self, jpg, id_persona):
        n = self.siguiente_foto
        self.siguiente_foto += 1
        if not self.guardar:
            return n
        hora = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        (FOTOS / f"{n:04d}_p{id_persona}_{hora}.jpg").write_bytes(jpg)
        return n


# Agrega una línea a fotos\registro.csv
def anotar_registro(r):
    nuevo = not REGISTRO.exists()
    with open(REGISTRO, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if nuevo:
            w.writerow(["hora", "resultado", "persona", "foto", "similitud"])
        sim = r.get("similitud")
        w.writerow([ahora_texto(), r["resultado"], r.get("persona", ""), r.get("foto", ""),
                    "" if sim is None else f"{sim:.3f}"])


# Convierte el resultado en la respuesta corta que lee el ESP32
def texto_respuesta(r):
    if r["resultado"] == "NUEVA":
        return f"NUEVA {r['persona']} {r['foto']}"
    if r["resultado"] == "CONOCIDA":
        return f"CONOCIDA {r['persona']} {r['hace']}"
    if r["resultado"] == "IR":
        return f"IR {r['dx']} {r['dy']}"
    return r["resultado"]


# Convierte el resultado en un mensaje para la consola
def texto_consola(r):
    sim = r.get("similitud")
    s = "" if sim is None else f" (similitud {sim:.2f})"
    if r["resultado"] == "NUEVA":
        return f"FOTO #{r['foto']} persona {r['persona']}{s}"
    if r["resultado"] == "CONOCIDA":
        return f"persona {r['persona']} ya registrada hace {r['hace']} s{s}"
    if r["resultado"] == "SIN_ROSTRO":
        return f"no hay rostro (FOMO se equivocó) - {r.get('detalle', '')}"
    if r["resultado"] == "REPETIR":
        return f"rostro nuevo de baja calidad ({r.get('detalle', '')}): pido otra foto"
    if r["resultado"] == "IR":
        extra = f" (foto mala: {r['detalle']})" if r.get("detalle") else ""
        return f"escaneo: rostro sin registrar a ({r['dx']}, {r['dy']}) px -> apuntar{extra}"
    if r["resultado"] in ("NADA", "CONOCIDAS"):
        return f"escaneo: {r['resultado'].lower()}"
    return f"ERROR: {r.get('detalle', '')}"


reconocedor = None   # el Reconocedor que usa el servidor


# Atiende las peticiones web: fotos del ESP32 (POST) y galería (GET)
class Manejador(BaseHTTPRequestHandler):
    # Desactiva el registro de cada petición en la consola
    def log_message(self, *args):
        pass

    # Envía una respuesta con su código, tipo y contenido
    def _responder(self, codigo, tipo, datos):
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(datos)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(datos)

    # Recibe una foto del ESP32 en /rostro o /escanear y responde la decisión
    def do_POST(self):
        ruta = urlparse(self.path).path
        if ruta not in ("/rostro", "/escanear"):
            return self._responder(404, "text/plain", b"no existe")
        largo = int(self.headers.get("Content-Length", 0))
        if largo <= 0 or largo > 2_000_000:
            return self._responder(400, "text/plain", b"ERROR tamano")
        jpg = self.rfile.read(largo)
        t0 = time.time()
        r = reconocedor.analizar(jpg) if ruta == "/rostro" else reconocedor.escanear(jpg)
        ms = int((time.time() - t0) * 1000)
        if r["resultado"] not in ("NADA", "CONOCIDAS"):
            print(f"[{ahora_texto()}] {self.client_address[0]}: {texto_consola(r)}  [{ms} ms]")
        if reconocedor.guardar and r["resultado"] in ("NUEVA", "CONOCIDA"):
            anotar_registro(r)
        self._responder(200, "text/plain", texto_respuesta(r).encode())

    # Muestra la galería (/), una foto (/foto/...) o el estado (/estado)
    def do_GET(self):
        ruta = urlparse(self.path).path
        if ruta == "/estado":
            return self._responder(200, "text/plain", b"OK")
        if ruta.startswith("/foto/"):
            nombre = unquote(ruta[len("/foto/"):])
            archivo = FOTOS / nombre
            if Path(nombre).name != nombre or not nombre.endswith(".jpg") or not archivo.is_file():
                return self._responder(404, "text/plain", b"foto no disponible")
            return self._responder(200, "image/jpeg", archivo.read_bytes())
        if ruta == "/":
            return self._responder(200, "text/html; charset=utf-8", galeria().encode())
        self._responder(404, "text/plain", b"no existe")


# Arma la página web con las últimas 80 fotos
def galeria():
    fotos = sorted(FOTOS.glob("*.jpg"), reverse=True)[:80]
    figuras = []
    for f in fotos:
        partes = f.stem.split("_")
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


# Devuelve la IP de la PC en la red local
def ip_local():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


# Analiza fotos de la PC en orden, como si llegaran del ESP32 (no guarda nada)
def probar(archivos):
    rec = Reconocedor(guardar=False)
    for nombre in archivos:
        ruta = Path(nombre)
        if not ruta.is_file():
            print(f"{nombre}: no existe")
            continue
        r = rec.analizar(ruta.read_bytes())
        print(f"{ruta.name}: {texto_respuesta(r):<16} | {texto_consola(r)}")


# Lee los argumentos e inicia el servidor (o el modo --probar)
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
