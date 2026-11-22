"""Estadística descriptiva de las pruebas de VigIA: lee la ficha y registro.csv, genera tablas y gráficos."""
import argparse
import csv
import statistics as st
import sys
from pathlib import Path

from openpyxl import load_workbook

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

AQUI = Path(__file__).resolve().parent                     # carpeta de este script
REGISTRO = AQUI.parent / "pc_servidor" / "fotos" / "registro.csv"   # decisiones guardadas por el servidor
META_DIF = 85.0          # % mínimo de diferenciación correcta (objetivo general)
META_LAT = 1000.0        # latencia máxima de inferencia (ms)
META_CENTRADO = 3.0      # tiempo máximo de centrado (s)
META_FALLIDOS = 2.0      # % máximo de envíos fallidos
RANGOS_LUX = [(0, 150, "< 150 lux"), (150, 300, "150–300 lux"), (300, 500, "300–500 lux"), (500, 1e9, "> 500 lux")]   # grupos de iluminación
COLOR = "#13A89E"        # color de las barras
COLOR2 = "#0F1B2D"       # color de la meta / segunda serie


# Devuelve las filas con datos de una hoja como lista de diccionarios
def leer_hoja(wb, nombre):
    ws = wb[nombre]
    filas = list(ws.iter_rows(values_only=True))
    claves = [str(c).strip() if c is not None else "" for c in filas[0]]
    datos = []
    for f in filas[1:]:
        d = {k: v for k, v in zip(claves, f)}
        datos.append(d)
    return datos


# Convierte un valor de celda a número (o None si está vacío)
def num(v):
    if v is None or str(v).strip() == "":
        return None
    try:
        return float(str(v).replace(",", "."))
    except ValueError:
        return None


# Normaliza Sí/No de la ficha
def si(v):
    return str(v).strip().lower() in ("sí", "si", "s", "yes")


# Media, desviación estándar, mínimo y máximo de una lista
def resumen(valores):
    if not valores:
        return None
    return {"n": len(valores), "media": st.mean(valores), "de": st.stdev(valores) if len(valores) > 1 else 0.0,
            "min": min(valores), "max": max(valores)}


# Porcentaje seguro (evita dividir entre cero)
def pct(a, b):
    return 100.0 * a / b if b else None


# Texto con formato para un porcentaje
def fp(v):
    return "sin datos" if v is None else f"{v:.1f} %"


# Busca en la hoja Costos el valor de la columna D al lado de una etiqueta
def valor_costos(ws, etiqueta):
    for fila in ws.iter_rows(values_only=True):
        if fila[0] and str(fila[0]).startswith(etiqueta):
            return fila[3]
    return None


# Calcula todos los indicadores a partir de la ficha
def analizar(ficha):
    wb = load_workbook(ficha, data_only=False)
    ensayos = [e for e in leer_hoja(wb, "Ensayos") if any(e.get(k) not in (None, "") for k in ("Fecha", "Iluminación (lux)", "Envíos a la PC"))]
    pasos = [p for p in leer_hoja(wb, "Pasos") if p.get("Persona (código)") and p.get("Respuesta del sistema")]
    tonos = {str(p["Código"]).strip(): p.get("Tono de piel") for p in leer_hoja(wb, "Participantes") if p.get("Código")}
    lat = [num(f.get("Latencia IA (ms)")) for f in leer_hoja(wb, "Latencia")]
    lat = [v for v in lat if v is not None]
    lux_de = {int(num(e["Ensayo"])): num(e.get("Iluminación (lux)")) for e in ensayos if num(e.get("Ensayo"))}

    r = {"ensayos": len(ensayos), "pasos": len(pasos)}

    # Diferenciación: solo pasos donde el sistema decidió NUEVA o CONOCIDA
    decididos = [p for p in pasos if str(p["Respuesta del sistema"]).upper() in ("NUEVA", "CONOCIDA") and p.get("¿Ya pasó en los últimos 5 min?")]
    mat = {("No", "NUEVA"): 0, ("No", "CONOCIDA"): 0, ("Sí", "NUEVA"): 0, ("Sí", "CONOCIDA"): 0}
    for p in decididos:
        real = "Sí" if si(p["¿Ya pasó en los últimos 5 min?"]) else "No"
        mat[(real, str(p["Respuesta del sistema"]).upper())] += 1
    correctas = mat[("No", "NUEVA")] + mat[("Sí", "CONOCIDA")]
    r["matriz"] = mat
    r["decisiones"] = len(decididos)
    r["correctas"] = correctas
    r["duplicados"] = mat[("Sí", "NUEVA")]
    r["confusiones"] = mat[("No", "CONOCIDA")]
    r["dif"] = pct(correctas, len(decididos))

    # Detección y centrado
    centrados = [p for p in pasos if si(p.get("¿Encontró y centró el rostro?"))]
    r["centrados"] = len(centrados)
    r["det"] = pct(len(centrados), len(pasos))
    tiempos = [num(p.get("Tiempo de centrado (s)")) for p in centrados]
    tiempos = [t for t in tiempos if t is not None]
    r["tiempo"] = resumen(tiempos)
    r["tiempo_ok"] = pct(sum(t < META_CENTRADO for t in tiempos), len(tiempos))
    r["tiempos"] = tiempos

    # Falsos positivos y envíos
    fp_total = sum(num(e.get("Falsos positivos")) or 0 for e in ensayos)
    r["fp"] = fp_total
    r["fp_tasa"] = pct(fp_total, fp_total + len(centrados))
    envios = sum(num(e.get("Envíos a la PC")) or 0 for e in ensayos)
    fallidos = sum(num(e.get("Envíos fallidos")) or 0 for e in ensayos)
    r["envios"], r["fallidos"] = envios, fallidos
    r["fallidos_tasa"] = pct(fallidos, envios + fallidos)

    # Latencia
    r["lat"] = resumen(lat)
    r["lats"] = lat

    # Por iluminación y por tono de piel
    r["por_lux"] = []
    for a, b, nombre in RANGOS_LUX:
        sel = [p for p in pasos if lux_de.get(int(num(p["Ensayo"]) or 0)) is not None and a <= lux_de[int(num(p["Ensayo"]))] < b]
        dec = [p for p in sel if p in decididos]
        ok = sum(1 for p in dec if (si(p["¿Ya pasó en los últimos 5 min?"]) and str(p["Respuesta del sistema"]).upper() == "CONOCIDA")
                 or (not si(p["¿Ya pasó en los últimos 5 min?"]) and str(p["Respuesta del sistema"]).upper() == "NUEVA"))
        if sel:
            r["por_lux"].append((nombre, len(sel), pct(sum(si(p.get("¿Encontró y centró el rostro?")) for p in sel), len(sel)), pct(ok, len(dec))))
    r["por_tono"] = []
    for tono in ("Claro", "Medio", "Oscuro"):
        sel = [p for p in pasos if tonos.get(str(p["Persona (código)"]).strip()) == tono]
        if sel:
            r["por_tono"].append((tono, len(sel), pct(sum(si(p.get("¿Encontró y centró el rostro?")) for p in sel), len(sel))))

    # Costos
    wc = wb["Costos"]
    total = 0.0
    for fila in wc.iter_rows(min_row=2, values_only=True):
        if fila[0] and num(fila[1]) is not None and num(fila[2]) is not None:
            total += num(fila[1]) * num(fila[2])
    r["costo"] = total or None
    r["cctv"] = num(valor_costos(wc, "Costo de un CCTV"))
    r["cctv_fuente"] = valor_costos(wc, "Fuente del precio")
    return r


# Lee registro.csv del servidor (decisiones y similitudes)
def leer_registro(ruta):
    if not ruta.exists():
        return None
    filas = list(csv.DictReader(open(ruta, encoding="utf-8")))
    sims = [float(f["similitud"]) for f in filas if f.get("similitud")]
    return {"total": len(filas), "nuevas": sum(f["resultado"] == "NUEVA" for f in filas),
            "conocidas": sum(f["resultado"] == "CONOCIDA" for f in filas),
            "personas": len({f["persona"] for f in filas}),
            "sim_conocidas": resumen([float(f["similitud"]) for f in filas if f["resultado"] == "CONOCIDA" and f.get("similitud")]),
            "sim_nuevas": resumen([float(f["similitud"]) for f in filas if f["resultado"] == "NUEVA" and f.get("similitud")]),
            "sims": sims}


# Indica si un valor cumple una meta (mayor o menor)
def cumple(valor, meta, mayor=True):
    if valor is None:
        return "sin datos"
    return "Cumple" if (valor > meta if mayor else valor < meta) else "No cumple"


# Dibuja y guarda los gráficos en la carpeta de salida
def graficos(r, reg, carpeta):
    hechos = []
    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False})

    if r["decisiones"]:
        m = r["matriz"]
        fig, ax = plt.subplots(figsize=(4.6, 3.8))
        datos = [[m[("No", "NUEVA")], m[("No", "CONOCIDA")]], [m[("Sí", "NUEVA")], m[("Sí", "CONOCIDA")]]]
        ax.imshow(datos, cmap="Greens")
        for i in range(2):
            for j in range(2):
                ax.text(j, i, datos[i][j], ha="center", va="center", fontsize=16, fontweight="bold")
        ax.set_xticks([0, 1], ["NUEVA", "CONOCIDA"])
        ax.set_yticks([0, 1], ["Primera vez", "Ya registrada"])
        ax.set_xlabel("Decisión del sistema")
        ax.set_ylabel("Situación real")
        ax.set_title("Matriz de confusión")
        fig.tight_layout()
        fig.savefig(carpeta / "1_matriz_confusion.png", dpi=200)
        plt.close(fig)
        hechos.append("1_matriz_confusion.png")

    if r["lats"]:
        fig, ax = plt.subplots(figsize=(6, 3.4))
        ax.hist(r["lats"], bins=12, color=COLOR, edgecolor="white")
        ax.axvline(META_LAT, color=COLOR2, ls="--", label=f"Meta < {META_LAT:.0f} ms")
        ax.axvline(r["lat"]["media"], color="#E8833A", label=f"Media {r['lat']['media']:.0f} ms")
        ax.set_xlabel("Latencia de inferencia (ms)")
        ax.set_ylabel("Frecuencia")
        ax.set_title("Latencia de la IA en el ESP32-CAM")
        ax.legend(frameon=False)
        fig.tight_layout()
        fig.savefig(carpeta / "2_latencia.png", dpi=200)
        plt.close(fig)
        hechos.append("2_latencia.png")

    if r["tiempos"]:
        fig, ax = plt.subplots(figsize=(6, 3.4))
        ax.hist(r["tiempos"], bins=10, color=COLOR, edgecolor="white")
        ax.axvline(META_CENTRADO, color=COLOR2, ls="--", label=f"Meta < {META_CENTRADO:.0f} s")
        ax.set_xlabel("Tiempo de centrado (s)")
        ax.set_ylabel("Frecuencia")
        ax.set_title("Tiempo para centrar el rostro")
        ax.legend(frameon=False)
        fig.tight_layout()
        fig.savefig(carpeta / "3_tiempo_centrado.png", dpi=200)
        plt.close(fig)
        hechos.append("3_tiempo_centrado.png")

    if r["por_lux"]:
        fig, ax = plt.subplots(figsize=(6, 3.4))
        nombres = [x[0] for x in r["por_lux"]]
        x = range(len(nombres))
        ax.bar([i - 0.2 for i in x], [x_[2] or 0 for x_ in r["por_lux"]], 0.4, color=COLOR, label="Detección y centrado")
        ax.bar([i + 0.2 for i in x], [x_[3] or 0 for x_ in r["por_lux"]], 0.4, color=COLOR2, label="Diferenciación correcta")
        ax.set_xticks(list(x), nombres)
        ax.set_ylim(0, 105)
        ax.set_ylabel("%")
        ax.set_title("Desempeño según la iluminación")
        ax.legend(frameon=False, loc="lower right")
        fig.tight_layout()
        fig.savefig(carpeta / "4_iluminacion.png", dpi=200)
        plt.close(fig)
        hechos.append("4_iluminacion.png")

    indicadores = [("Diferenciación\ncorrecta", r["dif"], META_DIF), ("Detección\ny centrado", r["det"], None),
                   ("Centrado\n< 3 s", r["tiempo_ok"], None)]
    indicadores = [i for i in indicadores if i[1] is not None]
    if indicadores:
        fig, ax = plt.subplots(figsize=(6, 3.4))
        ax.bar([i[0] for i in indicadores], [i[1] for i in indicadores], color=COLOR, width=0.55)
        for k, i in enumerate(indicadores):
            ax.text(k, i[1] + 1.5, f"{i[1]:.1f} %", ha="center")
        ax.axhline(META_DIF, color=COLOR2, ls="--", label=f"Meta de diferenciación ({META_DIF:.0f} %)")
        ax.set_ylim(0, 110)
        ax.set_ylabel("%")
        ax.set_title("Indicadores principales")
        ax.legend(frameon=False, loc="lower right")
        fig.tight_layout()
        fig.savefig(carpeta / "5_indicadores.png", dpi=200)
        plt.close(fig)
        hechos.append("5_indicadores.png")

    if reg and reg["sims"]:
        fig, ax = plt.subplots(figsize=(6, 3.4))
        ax.hist(reg["sims"], bins=15, color=COLOR, edgecolor="white")
        ax.axvline(0.363, color=COLOR2, ls="--", label="Umbral 0,363")
        ax.set_xlabel("Similitud coseno con la persona más parecida")
        ax.set_ylabel("Frecuencia")
        ax.set_title("Similitudes calculadas por SFace (registro.csv)")
        ax.legend(frameon=False)
        fig.tight_layout()
        fig.savefig(carpeta / "6_similitud.png", dpi=200)
        plt.close(fig)
        hechos.append("6_similitud.png")
    return hechos


# Formatea un resumen estadístico como texto
def txt_res(s, unidad, dec=1):
    if not s:
        return "sin datos"
    u = f" {unidad}" if unidad else ""
    return (f"media {s['media']:.{dec}f}{u}, DE {s['de']:.{dec}f}, mín {s['min']:.{dec}f}, "
            f"máx {s['max']:.{dec}f} (n = {s['n']})")


# Escribe resultados.md con tablas y fórmulas
def informe(r, reg, imgs, carpeta):
    L = ["# Resultados de las pruebas – VigIA", "",
         f"Ensayos registrados: **{r['ensayos']}** · Pasos de personas: **{r['pasos']}**", "",
         "## Tabla 1. Indicadores frente a las metas", "",
         "| Indicador | Fórmula | Resultado | Meta | Estado |", "|---|---|---|---|---|",
         f"| Diferenciación correcta | correctas / decisiones × 100 = {r['correctas']} / {r['decisiones']} × 100 | {fp(r['dif'])} | > {META_DIF:.0f} % | {cumple(r['dif'], META_DIF)} |",
         f"| Detección y centrado | centrados / pasos × 100 = {r['centrados']} / {r['pasos']} × 100 | {fp(r['det'])} | – | – |",
         f"| Tiempo de centrado | promedio de los tiempos | {txt_res(r['tiempo'], 's')} | < {META_CENTRADO:.0f} s | {cumple(r['tiempo']['media'] if r['tiempo'] else None, META_CENTRADO, False)} |",
         f"| Centrados en < 3 s | casos < 3 s / casos × 100 | {fp(r['tiempo_ok'])} | – | – |",
         f"| Falsos positivos | FP / (FP + detecciones) × 100 = {r['fp']:.0f} / ({r['fp']:.0f} + {r['centrados']}) × 100 | {fp(r['fp_tasa'])} | – | – |",
         f"| Latencia de inferencia | promedio de los valores 'IA' | {txt_res(r['lat'], 'ms', 0)} | < {META_LAT:.0f} ms | {cumple(r['lat']['media'] if r['lat'] else None, META_LAT, False)} |",
         f"| Envíos fallidos | fallidos / (envíos + fallidos) × 100 = {r['fallidos']:.0f} / {r['envios'] + r['fallidos']:.0f} × 100 | {fp(r['fallidos_tasa'])} | < {META_FALLIDOS:.0f} % | {cumple(r['fallidos_tasa'], META_FALLIDOS, False)} |",
         "", "## Tabla 2. Matriz de confusión (decisiones NUEVA / CONOCIDA)", "",
         "| Situación real \\ Sistema | NUEVA | CONOCIDA |", "|---|---|---|",
         f"| Primera vez | {r['matriz'][('No', 'NUEVA')]} (correcto) | {r['matriz'][('No', 'CONOCIDA')]} (confusión) |",
         f"| Ya registrada (< 5 min) | {r['matriz'][('Sí', 'NUEVA')]} (duplicado) | {r['matriz'][('Sí', 'CONOCIDA')]} (correcto) |",
         ""]
    if r["por_lux"]:
        L += ["## Tabla 3. Desempeño según la iluminación", "", "| Iluminación | Pasos | Detección y centrado | Diferenciación correcta |", "|---|---|---|---|"]
        L += [f"| {a} | {b} | {fp(c)} | {fp(d)} |" for a, b, c, d in r["por_lux"]]
        L.append("")
    if r["por_tono"]:
        L += ["## Tabla 4. Detección según el tono de piel", "", "| Tono de piel | Pasos | Detección y centrado |", "|---|---|---|"]
        L += [f"| {a} | {b} | {fp(c)} |" for a, b, c in r["por_tono"]]
        L.append("")
    costo = "sin datos" if r["costo"] is None else "S/ %.2f" % r["costo"]
    cctv = "sin datos" if r["cctv"] is None else "S/ %.2f" % r["cctv"]
    L += ["## Tabla 5. Costo", "", "| Concepto | Valor |", "|---|---|",
          f"| Costo total del prototipo | {costo} |",
          f"| CCTV comercial de referencia | {cctv} ({r['cctv_fuente'] or 'fuente sin anotar'}) |"]
    if r["costo"] and r["cctv"]:
        L.append(f"| Ahorro | S/ {r['cctv'] - r['costo']:.2f} ({pct(r['cctv'] - r['costo'], r['cctv']):.1f} %) |")
    L.append("")
    if reg:
        L += ["## Tabla 6. Registro automático del servidor (registro.csv)", "",
              "| Concepto | Valor |", "|---|---|",
              f"| Decisiones registradas | {reg['total']} |", f"| NUEVA | {reg['nuevas']} |", f"| CONOCIDA | {reg['conocidas']} |",
              f"| Personas distintas | {reg['personas']} |",
              f"| Similitud en CONOCIDA | {txt_res(reg['sim_conocidas'], '', 3)} |",
              f"| Similitud en NUEVA | {txt_res(reg['sim_nuevas'], '', 3)} |", ""]
    L += ["## Fórmulas usadas", "",
          "- Porcentaje: p = a / n × 100",
          "- Media: x̄ = Σxᵢ / n",
          "- Desviación estándar muestral: s = √( Σ(xᵢ − x̄)² / (n − 1) )", ""]
    if imgs:
        L += ["## Gráficos", ""] + [f"![{i}]({i})" for i in imgs]
    (carpeta / "resultados.md").write_text("\n".join(L), encoding="utf-8")
    return "\n".join(L)


# Lee los argumentos y ejecuta el análisis
def main():
    ap = argparse.ArgumentParser(description="Estadística descriptiva de las pruebas de VigIA.")
    ap.add_argument("--ficha", default=str(AQUI / "ficha_pruebas.xlsx"))
    ap.add_argument("--registro", default=str(REGISTRO))
    ap.add_argument("--salida", default=str(AQUI / "resultados"))
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    carpeta = Path(a.salida)
    carpeta.mkdir(parents=True, exist_ok=True)
    r = analizar(a.ficha)
    reg = leer_registro(Path(a.registro))
    imgs = graficos(r, reg, carpeta)
    print(informe(r, reg, imgs, carpeta))
    print(f"\nTablas en {carpeta / 'resultados.md'} y {len(imgs)} gráfico(s) en {carpeta}")


if __name__ == "__main__":
    main()
