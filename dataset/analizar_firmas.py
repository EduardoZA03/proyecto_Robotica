"""
VigIA - Análisis de la "firma de color" con fotos reales de la ESP32-CAM
--------------------------------------------------------------------------
Objetivo: medir en la PC (sin compilar 15 min ni probar con gente) qué tan
bien distingue la firma de color del 07 a una persona de otra, y elegir los
umbrales UMBRAL_MISMA / UMBRAL_IGNORAR con números en vez de adivinar.

NO entrena ninguna IA ni reconoce a nadie: solo calcula la misma firma que el
ESP32 (colores de la cabeza en 3 franjas + brillo) y compara fotos entre sí.

Qué hace:
  1. Lee las fotos de dataset\\muestra\\<persona>\\*.jpg (una carpeta por persona).
  2. Busca el rostro en cada foto (detector Haar de OpenCV, viene incluido).
  3. Calcula la firma IGUAL que calcularFirma() de 07_vigia_completo.ino.
     También prueba una versión "escalada" (franjas del tamaño del rostro)
     para ver si mejora la separación.
  4. Compara todas las parejas de fotos:
       misma persona   -> la similitud debería ser ALTA
       distinta persona-> la similitud debería ser BAJA
  5. Muestra una tabla por umbral: cuántas fotos se REPETIRÍAN y cuántas
     personas se PERDERÍAN, y recomienda un umbral.

Cómo tomar las fotos (ESP32-CAM con 06_captura_dataset, 640x480):
  Usar nombres genéricos (persona1, persona2...), NO nombres reales.
  En PowerShell, dentro de la carpeta dataset:
    powershell -ExecutionPolicy Bypass -File captura_fotos.ps1 -Ip 192.168.18.74 -Cantidad 15 -Intervalo 2 -Carpeta muestra\\persona1
  Mientras captura, la persona se mueve un poco: 1.5 a 3 m, de frente y algo
  de lado, con y sin lentes. Ideal: 8-10 personas, 10-20 fotos cada una, en
  el laboratorio y con bata (como en el uso real).

Uso:
  pip install "opencv-python<5" numpy matplotlib     (solo la primera vez)
  python analizar_firmas.py                 -> analiza dataset\\muestra
  python analizar_firmas.py --revisar       -> además guarda las fotos con
                                               el rostro y las franjas dibujadas
                                               en dataset\\muestra_revisar
  python analizar_firmas.py --grafico       -> guarda resultados_firmas.png
                                               (para el informe)

Errores comunes:
  "No module named cv2"       -> falta instalar: pip install "opencv-python<5"
  "haarcascade ... no existe" -> se instaló OpenCV 5 (ya no trae el detector):
                                 pip install "opencv-python<5"
  "sin rostro" en muchas fotos-> persona muy lejos, de perfil o a contraluz.
                                 Revisar con --revisar.
  Las fotos salen de 320x240  -> el ESP32 tiene el 06 viejo: subir el 06
                                 nuevo (640x480). El script las agranda igual,
                                 pero los números se parecen menos al 07.

Privacidad: las fotos se quedan en la PC (dataset\\muestra está en .gitignore).
Pedir permiso a cada persona y borrar las fotos al terminar el análisis.
"""

import argparse
import itertools
import sys
from pathlib import Path

import numpy as np

try:
    import cv2
except ImportError:
    sys.exit('Falta OpenCV. Instalar con:  pip install "opencv-python<5" numpy matplotlib')

# ---- Igual que en 07_vigia_completo.ino ----
CAM_ANCHO, CAM_ALTO = 640, 480
NFIRMA = 64
UMBRAL_MISMA_ACTUAL = 0.92     # rostro ya centrado
UMBRAL_IGNORAR_ACTUAL = 0.96   # rostro visto de lejos / descentrado

# Medidas de la cabeza que usa calcularFirma() (en píxeles de 640x480):
#   ancho: centro ± 45 ; franjas: -70 | -25 | +15 | +55 respecto del centro
MEDIO_ANCHO = 45
LIMITES = (-70, -25, 15, 55)
# Para la versión "escalada": ancho de rostro (detector Haar) para el que
# esas medidas fijas encajan bien. Un rostro más chico (más lejos) achica
# las franjas en la misma proporción.
ANCHO_REFERENCIA = 90

CARPETA_BASE = Path(__file__).resolve().parent


# ---------------------------------------------------------------------------
def leer_foto(ruta):
    """Lee un JPG (también con tildes en la ruta) y lo deja en 640x480 BGR."""
    datos = np.fromfile(str(ruta), dtype=np.uint8)   # cv2.imread falla con "ó"
    img = cv2.imdecode(datos, cv2.IMREAD_COLOR)
    if img is None:
        return None, False
    original = img.shape[1] == CAM_ANCHO and img.shape[0] == CAM_ALTO
    if not original:
        img = cv2.resize(img, (CAM_ANCHO, CAM_ALTO), interpolation=cv2.INTER_LINEAR)
    return img, original


def crear_detector():
    ruta = Path(cv2.data.haarcascades) / "haarcascade_frontalface_default.xml"
    if not ruta.exists():
        sys.exit('El haarcascade no existe (OpenCV 5). Instalar:  pip install "opencv-python<5"')
    return cv2.CascadeClassifier(str(ruta))


def detectar_rostro(detector, img):
    """Devuelve (x, y, ancho, alto) del rostro más grande, o None."""
    gris = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    caras = detector.detectMultiScale(gris, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30))
    if len(caras) == 0:
        return None
    return max(caras, key=lambda c: c[2] * c[3])


def zonas_firma(fx, fy, escala):
    """Rectángulos (x0, x1, y0, y1) de las 3 franjas, recortados a la imagen."""
    lim = [fy + int(round(d * escala)) for d in LIMITES]
    medio = int(round(MEDIO_ANCHO * escala))
    x0, x1 = max(0, fx - medio), min(CAM_ANCHO - 1, fx + medio)
    return [(x0, x1, max(0, lim[f]), min(CAM_ALTO - 1, lim[f + 1])) for f in range(3)]


def calcular_firma(img, fx, fy, escala=1.0):
    """
    Copia de calcularFirma() del ESP32 (con escala=1.0 es idéntica).
    img está en BGR, igual que el buffer del ESP32 (px[0]=B, px[1]=G, px[2]=R).
    """
    firma = np.zeros(NFIRMA)
    total_franja = [0, 0, 0]
    total_brillo = 0
    for f, (x0, x1, y0, y1) in enumerate(zonas_firma(fx, fy, escala)):
        if y1 <= y0:
            continue
        # En el ESP32: y de y0 a y1-1 de a 2; x de x0 a x1 (incluido) de a 2
        px = img[y0:y1:2, x0:x1 + 1:2].reshape(-1, 3).astype(np.int32)
        suma = px.sum(axis=1)
        np.add.at(firma, 48 + np.minimum(15, suma // 48), 1)       # brillo
        total_brillo += len(suma)
        ok = suma >= 60                                              # muy oscuro: se salta
        r = (px[ok, 2] * 4) // (suma[ok] + 1)
        g = (px[ok, 1] * 4) // (suma[ok] + 1)
        np.add.at(firma, f * 16 + r * 4 + g, 1)
        total_franja[f] += int(ok.sum())
    for f in range(3):
        if total_franja[f]:
            firma[f * 16:(f + 1) * 16] *= 0.25 / total_franja[f]
    if total_brillo:
        firma[48:] *= 0.25 / total_brillo
    return firma


def similitud(a, b):
    """1.0 = colores idénticos, 0.0 = nada en común (igual que en el ESP32)."""
    return float(np.minimum(a, b).sum())


# ---------------------------------------------------------------------------
def cargar_muestra(carpeta, revisar):
    """Lee todas las fotos y devuelve {persona: [(archivo, firma_actual, firma_escalada)]}."""
    detector = crear_detector()
    personas = {}
    sin_rostro, chicas = [], 0
    carpeta_revisar = CARPETA_BASE / (carpeta.name + "_revisar")

    for dir_persona in sorted(p for p in carpeta.iterdir() if p.is_dir()):
        fotos = sorted(list(dir_persona.glob("*.jpg")) + list(dir_persona.glob("*.jpeg")))
        for ruta in fotos:
            img, original = leer_foto(ruta)
            if img is None:
                print(f"  (no se pudo leer {ruta.name})")
                continue
            chicas += not original
            cara = detectar_rostro(detector, img)
            if cara is None:
                sin_rostro.append(f"{dir_persona.name}/{ruta.name}")
                continue
            x, y, w, h = (int(v) for v in cara)
            fx, fy = x + w // 2, y + h // 2
            escala = w / ANCHO_REFERENCIA
            personas.setdefault(dir_persona.name, []).append((
                ruta.name,
                calcular_firma(img, fx, fy, 1.0),
                calcular_firma(img, fx, fy, escala),
            ))
            if revisar:
                guardar_revision(img, (x, y, w, h), fx, fy, escala,
                                 carpeta_revisar / dir_persona.name / ruta.name)

    if chicas:
        print(f"AVISO: {chicas} fotos no son de 640x480 (¿06 viejo?). Se agrandaron.")
    if sin_rostro:
        print(f"AVISO: {len(sin_rostro)} fotos sin rostro detectado (no se usan):")
        for nombre in sin_rostro[:10]:
            print(f"   - {nombre}")
        if len(sin_rostro) > 10:
            print(f"   ... y {len(sin_rostro) - 10} más")
    if revisar:
        print(f"Fotos para revisar en: {carpeta_revisar}")
    return personas


def guardar_revision(img, cara, fx, fy, escala, destino):
    """Dibuja el rostro (verde), las franjas actuales (rojo) y escaladas (azul)."""
    copia = img.copy()
    x, y, w, h = cara
    cv2.rectangle(copia, (x, y), (x + w, y + h), (0, 200, 0), 2)
    for esc, color in ((1.0, (0, 0, 255)), (escala, (255, 120, 0))):
        for (x0, x1, y0, y1) in zonas_firma(fx, fy, esc):
            cv2.rectangle(copia, (x0, y0), (x1, y1), color, 1)
    destino.parent.mkdir(parents=True, exist_ok=True)
    ok, datos = cv2.imencode(".jpg", copia)
    if ok:
        datos.tofile(str(destino))   # tofile: funciona con tildes en la ruta


def comparar(personas, indice):
    """Similitudes de todas las parejas: misma persona y distinta persona."""
    misma, distinta = [], []
    peor_pareja = {}   # (A, B) -> similitud más alta entre fotos de A y de B
    nombres = sorted(personas)
    for nombre in nombres:
        for a, b in itertools.combinations(personas[nombre], 2):
            misma.append(similitud(a[indice], b[indice]))
    for na, nb in itertools.combinations(nombres, 2):
        for a in personas[na]:
            for b in personas[nb]:
                s = similitud(a[indice], b[indice])
                distinta.append(s)
                peor_pareja[(na, nb)] = max(peor_pareja.get((na, nb), 0), s)
    return np.array(misma), np.array(distinta), peor_pareja


def tasas(misma, distinta, umbral):
    """% de fotos repetidas (misma persona tomada como nueva) y % de personas
    perdidas (persona distinta tomada como ya registrada)."""
    repetidas = 100 * np.mean(misma < umbral)
    perdidas = 100 * np.mean(distinta >= umbral)
    return repetidas, perdidas


def umbral_recomendado(misma, distinta):
    """
    Si no se cruzan: el punto medio del hueco (margen para personas nuevas).
    Si se cruzan: el proyecto prefiere repetir fotos antes que perder a alguien,
    así que se toma el umbral más bajo con 0 % de perdidas, + 0.01 de margen.
    """
    if misma.min() > distinta.max():
        return round((misma.min() + distinta.max()) / 2, 2)
    candidatos = np.round(np.arange(0.70, 1.00, 0.01), 2)
    sin_perdidas = [u for u in candidatos if tasas(misma, distinta, u)[1] == 0]
    if sin_perdidas:
        return min(0.99, round(sin_perdidas[0] + 0.01, 2))
    return min(candidatos, key=lambda u: tasas(misma, distinta, u)[1])


def informe(titulo, misma, distinta, peor_pareja):
    print(f"\n=== {titulo} ===")
    print(f"{'':16}{'mín':>6}{'prom':>7}{'máx':>6}   parejas")
    print(f"{'Misma persona':16}{misma.min():6.2f}{misma.mean():7.2f}{misma.max():6.2f}   {len(misma)}")
    print(f"{'Distinta':16}{distinta.min():6.2f}{distinta.mean():7.2f}{distinta.max():6.2f}   {len(distinta)}")

    if misma.min() > distinta.max():
        print(f"-> NO se cruzan: cualquier umbral entre {distinta.max():.2f} y {misma.min():.2f} separa perfecto.")
    else:
        print(f"-> SE CRUZAN entre {misma.min():.2f} y {distinta.max():.2f}: algún error es inevitable.")

    print(f"\n{'umbral':>7} {'fotos repetidas':>16} {'personas perdidas':>18}")
    for u in np.round(np.arange(0.80, 0.995, 0.02), 2):
        rep, perd = tasas(misma, distinta, u)
        marca = []
        if abs(u - UMBRAL_MISMA_ACTUAL) < 1e-6:
            marca.append("<- UMBRAL_MISMA actual")
        print(f"{u:7.2f} {rep:15.0f}% {perd:17.1f}%  {' '.join(marca)}")
    for nombre, u in (("UMBRAL_MISMA", UMBRAL_MISMA_ACTUAL), ("UMBRAL_IGNORAR", UMBRAL_IGNORAR_ACTUAL)):
        rep, perd = tasas(misma, distinta, u)
        print(f"Actual {nombre} {u:.2f}: repite {rep:.0f}% / pierde {perd:.1f}%")

    u = umbral_recomendado(misma, distinta)
    rep, perd = tasas(misma, distinta, u)
    print(f"Recomendado: {u:.2f}  (repite {rep:.0f}%, pierde {perd:.1f}%)")

    print("\nParejas que más se confunden (similitud más alta entre 2 personas):")
    for (a, b), s in sorted(peor_pareja.items(), key=lambda kv: -kv[1])[:5]:
        aviso = "  <- se confunden con el umbral actual" if s >= UMBRAL_MISMA_ACTUAL else ""
        print(f"  {a} vs {b}: {s:.2f}{aviso}")
    return u


def grafico(resultados, destino):
    """Histograma misma vs distinta persona, para el informe."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    AZUL, NARANJA = "#2a78d6", "#eb6834"      # paleta validada (orden fijo)
    TINTA, TINTA_SUAVE, REJILLA = "#1f1f1e", "#6b6a64", "#e4e3dc"
    minimo = min(min(m.min(), d.min()) for _, m, d, _ in resultados)
    bins = np.arange(min(0.50, np.floor(minimo * 20) / 20), 1.0001, 0.01)

    fig, ejes = plt.subplots(1, len(resultados), figsize=(6 * len(resultados), 4), sharey=True)
    ejes = np.atleast_1d(ejes)
    for eje, (titulo, misma, distinta, recomendado) in zip(ejes, resultados):
        eje.hist(distinta, bins=bins, color=NARANJA, alpha=0.75, label="Personas distintas",
                 weights=np.full(len(distinta), 100 / len(distinta)), edgecolor="white", linewidth=0.5)
        eje.hist(misma, bins=bins, color=AZUL, alpha=0.75, label="Misma persona",
                 weights=np.full(len(misma), 100 / len(misma)), edgecolor="white", linewidth=0.5)
        # Etiquetas a distinta altura: no se tapan si los umbrales están juntos
        for u, texto, estilo, altura in ((UMBRAL_MISMA_ACTUAL, "actual", "--", 0.97),
                                         (recomendado, "recomendado", "-", 0.88)):
            eje.axvline(u, color=TINTA_SUAVE, linestyle=estilo, linewidth=1.5)
            eje.text(u, eje.get_ylim()[1] * altura, f" {texto} {u:.2f}", color=TINTA,
                     fontsize=9, va="top")
        eje.set_title(titulo, color=TINTA, fontsize=11, loc="left")
        eje.set_xlabel("Similitud de la firma (1.0 = idéntica)", color=TINTA_SUAVE)
        eje.grid(axis="y", color=REJILLA, linewidth=0.8)
        eje.set_axisbelow(True)
        for lado in ("top", "right"):
            eje.spines[lado].set_visible(False)
        for lado in ("left", "bottom"):
            eje.spines[lado].set_color(REJILLA)
        eje.tick_params(colors=TINTA_SUAVE)
    ejes[0].set_ylabel("% de parejas de fotos", color=TINTA_SUAVE)
    ejes[0].legend(frameon=False, loc="upper left")
    fig.tight_layout()
    fig.savefig(destino, dpi=150, facecolor="white")
    print(f"\nGráfico guardado en: {destino}")


# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Analiza la firma de color del 07 con fotos reales.")
    parser.add_argument("--carpeta", default="muestra", help="carpeta dentro de dataset (por defecto: muestra)")
    parser.add_argument("--revisar", action="store_true", help="guardar fotos con el rostro y las franjas dibujadas")
    parser.add_argument("--grafico", action="store_true", help="guardar resultados_firmas.png")
    args = parser.parse_args()

    carpeta = CARPETA_BASE / args.carpeta
    if not carpeta.is_dir():
        sys.exit(f"No existe {carpeta}\nCrear una subcarpeta por persona (persona1, persona2...) con sus fotos.")

    print(f"Leyendo fotos de {carpeta} ...")
    personas = cargar_muestra(carpeta, args.revisar)
    personas = {n: f for n, f in personas.items() if len(f) >= 2}
    print(f"Personas útiles (con 2 o más fotos): {len(personas)}")
    for nombre, fotos in sorted(personas.items()):
        print(f"  {nombre}: {len(fotos)} fotos")
    if len(personas) < 2:
        sys.exit("Se necesitan al menos 2 personas con 2 fotos cada una.")

    resultados = []
    for indice, titulo in ((1, "Firma ACTUAL del ESP32 (franjas fijas)"),
                           (2, "Firma ESCALADA al tamaño del rostro (propuesta)")):
        misma, distinta, peor = comparar(personas, indice)
        recomendado = informe(titulo, misma, distinta, peor)
        resultados.append((titulo, misma, distinta, recomendado))

    # ¿Cuál separa mejor? Menos personas perdidas con su umbral recomendado,
    # y a igualdad, menos fotos repetidas.
    puntajes = [tasas(m, d, u)[::-1] for _, m, d, u in resultados]
    mejor = min(range(len(resultados)), key=lambda i: puntajes[i])
    print(f"\n>>> Separa mejor: {resultados[mejor][0]}")

    if args.grafico:
        grafico(resultados, CARPETA_BASE / "resultados_firmas.png")


if __name__ == "__main__":
    main()
