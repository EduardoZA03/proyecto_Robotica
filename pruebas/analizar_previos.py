"""Estadística descriptiva de las pruebas ya realizadas (14 oct y 12 nov) a partir de datos_previos/."""
import csv
import statistics as st
import sys
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

AQUI = Path(__file__).resolve().parent          # carpeta de este script
DATOS = AQUI / "datos_previos"                   # datos de las pruebas anteriores
SALIDA = AQUI / "resultados_previos"             # carpeta de tablas y gráficos
VENTANA_S = 5 * 60                               # ventana de 5 minutos (s)
META = 85.0                                      # meta de diferenciación (%)
COLOR, COLOR2, COLOR3 = "#13A89E", "#0F1B2D", "#E8833A"   # colores de los gráficos


# Lee un CSV como lista de diccionarios
def leer(nombre):
    return list(csv.DictReader(open(DATOS / nombre, encoding="utf-8")))


# Media, desviación estándar, mínimo, máximo y n
def resumen(v):
    return {"n": len(v), "media": st.mean(v), "de": st.stdev(v) if len(v) > 1 else 0.0, "min": min(v), "max": max(v)}


# Porcentaje con una decimal
def pct(a, b):
    return 100.0 * a / b if b else 0.0


# Clasifica cada foto: correcta, duplicada (y por qué) e identificación correcta
def evaluar_fotos(fotos):
    ultima = {}                  # última foto (hora) de cada persona real por sesión
    primer_id = {}               # número que recibió cada persona en su primera foto
    duenio = {}                  # persona dueña de cada número
    for f in fotos:
        s, p, n = f["sesion"], f["persona_real"], f["id_asignado"]
        if f["reinicio_servidor"] == "si":
            primer_id = {k: v for k, v in primer_id.items() if k[0] != s}
            duenio = {k: v for k, v in duenio.items() if k[0] != s}
        h = datetime.strptime(f["hora"], "%H:%M:%S")
        previa = ultima.get((s, p))
        f["duplicada"] = previa is not None and (h - previa).total_seconds() < VENTANA_S
        f["causa"] = "" if not f["duplicada"] else ("reinicio del servidor" if f["reinicio_servidor"] == "si" else "error de reconocimiento")
        ultima[(s, p)] = h
        esperado = primer_id.setdefault((s, p), n)
        ok_id = esperado == n and duenio.get((s, n), p) == p
        duenio.setdefault((s, n), p)
        f["id_ok"] = ok_id
    return fotos


# Separa las filas de registro por sesión según la fecha y hora
def decisiones_por_sesion():
    reg12 = leer("registro_sesiones_1_y_2.csv")
    reg3 = leer("registro_sesion_3.csv")
    s1 = [r for r in reg12 if r["hora"].startswith("2026-10-14")]
    s2 = [r for r in reg12 if r["hora"].startswith("2026-11-12")]
    return {"1": s1, "2": s2, "3": reg3}


# Guarda un gráfico con formato común
def guardar(fig, nombre):
    fig.tight_layout()
    fig.savefig(SALIDA / nombre, dpi=200)
    plt.close(fig)


# Calcula indicadores, escribe tablas y dibuja gráficos
def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    SALIDA.mkdir(exist_ok=True)
    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False})
    sesiones = {s["sesion"]: s for s in leer("sesiones.csv")}
    fotos = evaluar_fotos(leer("verificacion_fotos.csv"))
    dec = decisiones_por_sesion()
    lat = [int(r["latencia_ms"]) for r in leer("latencias_monitor_serie.csv")]

    filas = []
    for s, info in sesiones.items():
        fs = [f for f in fotos if f["sesion"] == s]
        d = dec[s]
        nuevas = sum(r["resultado"] == "NUEVA" for r in d)
        conocidas = sum(r["resultado"] == "CONOCIDA" for r in d)
        sin = int(info["sin_rostro"])
        filas.append({
            "s": s, "info": info, "fotos": len(fs), "correctas": sum(not f["duplicada"] for f in fs),
            "dup_err": sum(f["causa"] == "error de reconocimiento" for f in fs),
            "dup_rei": sum(f["causa"] == "reinicio del servidor" for f in fs),
            "dudosas": sum(f["confianza"] == "dudosa" for f in fs),
            "id_ok": sum(f["id_ok"] for f in fs), "nuevas": nuevas, "conocidas": conocidas, "sin": sin,
            "personas": len({f["persona_real"] for f in fs})})
    tot = {k: sum(f[k] for f in filas) for k in ("fotos", "correctas", "dup_err", "dup_rei", "dudosas", "id_ok", "nuevas", "conocidas", "sin")}
    una = [f for f in filas if f["info"]["personas_por_encuadre"] == "Una"]
    una_t = {k: sum(f[k] for f in una) for k in ("fotos", "correctas")}
    varias = [f for f in filas if f["info"]["personas_por_encuadre"] != "Una"]
    var_t = {k: sum(f[k] for f in varias) for k in ("fotos", "correctas")}
    sim_c = [float(r["similitud"]) for d in dec.values() for r in d if r["resultado"] == "CONOCIDA" and r["similitud"]]
    sim_n = [float(r["similitud"]) for d in dec.values() for r in d if r["resultado"] == "NUEVA" and r["similitud"]]
    rl, rc, rn = resumen(lat), resumen(sim_c), resumen(sim_n)
    sin_err = tot["fotos"] - tot["dup_rei"]

    L = ["# Resultados de las pruebas realizadas (14 oct y 12 nov 2026)", "",
         "Fuente: fotos guardadas por el servidor (verificadas una por una), registro.csv, carpeta 'descartadas' y monitor serie del ESP32.", "",
         "## Tabla A. Sesiones de prueba", "",
         "| Sesión | Fecha | Horario | Iluminación | Personas por encuadre | Participantes | Fotos | Decisiones (NUEVA / CONOCIDA) | Sin rostro |",
         "|---|---|---|---|---|---|---|---|---|"]
    for f in filas:
        i = f["info"]
        L.append(f"| {f['s']} | {i['fecha']} | {i['horario']} | {i['iluminacion']} | {i['personas_por_encuadre']} | {f['personas']} | {f['fotos']} | {f['nuevas']} / {f['conocidas']} | {f['sin']} |")
    L.append(f"| **Total** | | | | | | **{tot['fotos']}** | **{tot['nuevas']} / {tot['conocidas']}** | **{tot['sin']}** |")
    L += ["", "## Tabla B. Registro único (fotos no redundantes)", "",
          "| Sesión | Fotos | Correctas | Duplicadas por error | Duplicadas por reinicio | Registro único | Identificación correcta |",
          "|---|---|---|---|---|---|---|"]
    for f in filas:
        L.append(f"| {f['s']} | {f['fotos']} | {f['correctas']} | {f['dup_err']} | {f['dup_rei']} | {pct(f['correctas'], f['fotos']):.1f} % | {f['id_ok']}/{f['fotos']} ({pct(f['id_ok'], f['fotos']):.1f} %) |")
    L.append(f"| **Total** | {tot['fotos']} | {tot['correctas']} | {tot['dup_err']} | {tot['dup_rei']} | **{pct(tot['correctas'], tot['fotos']):.1f} %** | {tot['id_ok']}/{tot['fotos']} ({pct(tot['id_ok'], tot['fotos']):.1f} %) |")
    L += ["",
          f"- Una persona por encuadre (sesiones 1 y 2): {una_t['correctas']}/{una_t['fotos']} = **{pct(una_t['correctas'], una_t['fotos']):.1f} %**",
          f"- Varias personas por encuadre (sesión 3): {var_t['correctas']}/{var_t['fotos']} = **{pct(var_t['correctas'], var_t['fotos']):.1f} %**",
          f"- Total sin contar los duplicados causados por reinicios del servidor: {tot['correctas']}/{sin_err} = **{pct(tot['correctas'], sin_err):.1f} %**",
          f"- Fotos con identificación dudosa (revisar): {tot['dudosas']}", "",
          "## Tabla C. Otros indicadores", "", "| Indicador | Cálculo | Resultado | Meta | Estado |", "|---|---|---|---|---|",
          f"| Latencia de inferencia (ESP32-CAM) | media de {rl['n']} lecturas del monitor serie | {rl['media']:.0f} ms (DE {rl['de']:.0f}; {rl['min']}–{rl['max']} ms) | < 1000 ms | {'Cumple' if rl['media'] < 1000 else 'No cumple'} |",
          f"| Falsos positivos de FOMO | sin rostro / (sin rostro + decisiones) = {tot['sin']} / {tot['sin'] + tot['nuevas'] + tot['conocidas']} | {pct(tot['sin'], tot['sin'] + tot['nuevas'] + tot['conocidas']):.1f} % | – | – |",
          f"| Similitud en CONOCIDA | media de {rc['n']} valores | {rc['media']:.3f} (DE {rc['de']:.3f}; {rc['min']:.3f}–{rc['max']:.3f}) | ≥ 0,363 | – |",
          f"| Similitud en NUEVA | media de {rn['n']} valores | {rn['media']:.3f} (DE {rn['de']:.3f}; {rn['min']:.3f}–{rn['max']:.3f}) | – | – |",
          "", "## Detalle de cada foto", "", "| Sesión | Foto | Hora | Nº asignado | Persona real | Confianza | Duplicada | Identificación | Nota |", "|---|---|---|---|---|---|---|---|---|"]
    for f in fotos:
        L.append(f"| {f['sesion']} | {f['foto']} | {f['hora']} | {f['id_asignado']} | {f['persona_real']} | {f['confianza']} | {'Sí – ' + f['causa'] if f['duplicada'] else 'No'} | {'Correcta' if f['id_ok'] else 'Incorrecta'} | {f['nota']} |")
    (SALIDA / "resultados_previos.md").write_text("\n".join(L), encoding="utf-8")
    print("\n".join(L))

    # Gráfico 1: registro único por condición
    fig, ax = plt.subplots(figsize=(6, 3.4))
    cats = ["Una persona\n(sesiones 1 y 2)", "Varias personas\n(sesión 3)", "Total"]
    vals = [pct(una_t["correctas"], una_t["fotos"]), pct(var_t["correctas"], var_t["fotos"]), pct(tot["correctas"], tot["fotos"])]
    ax.bar(cats, vals, color=[COLOR, COLOR3, COLOR2], width=0.55)
    for k, v in enumerate(vals):
        ax.text(k, v + 1.5, f"{v:.1f} %", ha="center")
    ax.axhline(META, color="black", ls="--", lw=1, label=f"Meta {META:.0f} %")
    ax.set_ylim(0, 110)
    ax.set_ylabel("Fotos no redundantes (%)")
    ax.set_title("Registro único según las personas en el encuadre")
    ax.legend(frameon=False, loc="lower right")
    guardar(fig, "fig_registro_unico.png")

    # Gráfico 2: latencia
    fig, ax = plt.subplots(figsize=(6, 3.4))
    ax.hist(lat, bins=24, color=COLOR, edgecolor="white")
    ax.axvline(rl["media"], color=COLOR3, label=f"Media {rl['media']:.0f} ms")
    ax.set_xlabel("Latencia de inferencia (ms)")
    ax.set_ylabel("Frecuencia")
    ax.set_title(f"Latencia de FOMO en el ESP32-CAM (n = {rl['n']})")
    ax.legend(frameon=False)
    guardar(fig, "fig_latencia.png")

    # Gráfico 3: similitud
    fig, ax = plt.subplots(figsize=(6, 3.4))
    bins = [i / 20 for i in range(0, 21)]
    ax.hist(sim_c, bins=bins, color=COLOR, alpha=0.85, label="CONOCIDA")
    ax.hist(sim_n, bins=bins, color=COLOR3, alpha=0.85, label="NUEVA")
    ax.axvline(0.363, color="black", ls="--", lw=1, label="Umbral 0,363")
    ax.set_xlabel("Similitud coseno (SFace)")
    ax.set_ylabel("Frecuencia")
    ax.set_title("Similitud con la persona más parecida")
    ax.legend(frameon=False)
    guardar(fig, "fig_similitud.png")

    # Gráfico 4: composición de las fotos por sesión
    fig, ax = plt.subplots(figsize=(6, 3.4))
    x = [f"Sesión {f['s']}" for f in filas]
    a = [f["correctas"] for f in filas]
    b = [f["dup_err"] for f in filas]
    c = [f["dup_rei"] for f in filas]
    ax.bar(x, a, color=COLOR, label="Correctas")
    ax.bar(x, b, bottom=a, color=COLOR3, label="Duplicadas (reconocimiento)")
    ax.bar(x, c, bottom=[i + j for i, j in zip(a, b)], color="#94A3B8", label="Duplicadas (reinicio)")
    ax.set_ylabel("Fotos")
    ax.set_title("Fotos tomadas por sesión")
    ax.legend(frameon=False)
    guardar(fig, "fig_fotos_sesion.png")
    print(f"\nTablas y gráficos en {SALIDA}")


if __name__ == "__main__":
    main()
