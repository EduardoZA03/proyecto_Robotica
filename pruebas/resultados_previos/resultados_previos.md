# Resultados de las pruebas realizadas (14 oct y 12 nov 2026)

Fuente: fotos guardadas por el servidor (verificadas una por una), registro.csv, carpeta 'descartadas' y monitor serie del ESP32.

## Tabla A. Sesiones de prueba

| Sesión | Fecha | Horario | Iluminación | Personas por encuadre | Participantes | Fotos | Decisiones (NUEVA / CONOCIDA) | Sin rostro |
|---|---|---|---|---|---|---|---|---|
| 1 | 2026-10-14 | 20:00-21:35 | Artificial (noche) | Una | 3 | 13 | 14 / 41 | 1 |
| 2 | 2026-11-12 | 10:12-10:16 | Natural (mañana) | Una | 3 | 3 | 3 / 4 | 2 |
| 3 | 2026-11-12 | 13:36-16:23 | Natural (tarde) | Varias (2 a 3) | 4 | 17 | 17 / 22 | 3 |
| **Total** | | | | | | **33** | **34 / 67** | **6** |

## Tabla B. Registro único (fotos no redundantes)

| Sesión | Fotos | Correctas | Duplicadas por error | Duplicadas por reinicio | Registro único | Identificación correcta |
|---|---|---|---|---|---|---|
| 1 | 13 | 11 | 0 | 2 | 84.6 % | 10/13 (76.9 %) |
| 2 | 3 | 3 | 0 | 0 | 100.0 % | 3/3 (100.0 %) |
| 3 | 17 | 11 | 6 | 0 | 64.7 % | 9/17 (52.9 %) |
| **Total** | 33 | 25 | 6 | 2 | **75.8 %** | 22/33 (66.7 %) |

- Una persona por encuadre (sesiones 1 y 2): 14/16 = **87.5 %**
- Varias personas por encuadre (sesión 3): 11/17 = **64.7 %**
- Total sin contar los duplicados causados por reinicios del servidor: 25/31 = **80.6 %**
- Fotos con identificación dudosa (revisar): 4

## Tabla C. Otros indicadores

| Indicador | Cálculo | Resultado | Meta | Estado |
|---|---|---|---|---|
| Latencia de inferencia (ESP32-CAM) | media de 873 lecturas del monitor serie | 707 ms (DE 13; 691–738 ms) | < 1000 ms | Cumple |
| Falsos positivos de FOMO | sin rostro / (sin rostro + decisiones) = 6 / 107 | 5.6 % | – | – |
| Similitud en CONOCIDA | media de 67 valores | 0.712 (DE 0.144; 0.378–0.935) | ≥ 0,363 | – |
| Similitud en NUEVA | media de 28 valores | 0.486 (DE 0.236; 0.135–0.902) | – | – |

## Detalle de cada foto

| Sesión | Foto | Hora | Nº asignado | Persona real | Confianza | Duplicada | Identificación | Nota |
|---|---|---|---|---|---|---|---|---|
| 1 | 1 | 20:00:14 | 1 | H1 | alta | No | Correcta | primera foto tras iniciar el servidor |
| 1 | 2 | 20:18:00 | 1 | H1 | alta | No | Correcta |  |
| 1 | 3 | 20:21:38 | 2 | N1 | alta | No | Correcta |  |
| 1 | 4 | 20:30:55 | 1 | H1 | alta | No | Correcta |  |
| 1 | 5 | 20:31:18 | 2 | M1 | alta | No | Incorrecta | recibio el mismo numero que N1 |
| 1 | 6 | 21:05:11 | 1 | H1 | alta | No | Correcta |  |
| 1 | 7 | 21:06:36 | 2 | M1 | alta | No | Incorrecta |  |
| 1 | 8 | 21:14:08 | 1 | H1 | alta | No | Correcta |  |
| 1 | 9 | 21:17:58 | 3 | M1 | alta | No | Incorrecta | recibio un numero nuevo aunque ya estaba registrada |
| 1 | 10 | 21:23:42 | 1 | H1 | alta | No | Correcta |  |
| 1 | 11 | 21:28:14 | 1 | H1 | alta | Sí – reinicio del servidor | Correcta | servidor reiniciado: perdio la memoria |
| 1 | 12 | 21:33:29 | 1 | H1 | alta | No | Correcta |  |
| 1 | 13 | 21:34:57 | 1 | H1 | alta | Sí – reinicio del servidor | Correcta | servidor reiniciado: perdio la memoria |
| 2 | 14 | 10:12:01 | 1 | H2 | alta | No | Correcta | primera foto tras iniciar el servidor |
| 2 | 15 | 10:13:02 | 2 | M2 | alta | No | Correcta |  |
| 2 | 16 | 10:14:02 | 3 | M3 | alta | No | Correcta |  |
| 3 | 1 | 13:36:09 | 1 | A | alta | No | Correcta | primera foto tras iniciar el servidor |
| 3 | 2 | 13:37:25 | 2 | B | alta | No | Correcta |  |
| 3 | 3 | 13:38:02 | 3 | A | dudosa | Sí – error de reconocimiento | Incorrecta | rostro cortado en el borde |
| 3 | 4 | 13:43:10 | 4 | A | alta | No | Incorrecta | recibio un numero nuevo aunque ya estaba registrado |
| 3 | 5 | 13:43:52 | 2 | B | alta | No | Correcta |  |
| 3 | 6 | 15:28:27 | 5 | C | alta | No | Correcta |  |
| 3 | 7 | 15:28:55 | 1 | A | alta | No | Correcta |  |
| 3 | 8 | 15:29:10 | 6 | D | dudosa | No | Correcta | dos personas en el encuadre |
| 3 | 9 | 15:30:26 | 7 | A | dudosa | Sí – error de reconocimiento | Incorrecta | tres personas en el encuadre |
| 3 | 10 | 15:30:56 | 8 | C | dudosa | Sí – error de reconocimiento | Incorrecta | foto borrosa |
| 3 | 11 | 15:31:13 | 4 | D | alta | Sí – error de reconocimiento | Incorrecta | recibio el numero de A |
| 3 | 12 | 15:31:47 | 9 | D | alta | Sí – error de reconocimiento | Incorrecta |  |
| 3 | 13 | 15:32:15 | 2 | B | alta | No | Correcta |  |
| 3 | 14 | 16:20:43 | 2 | A | alta | No | Incorrecta | recibio el numero de B |
| 3 | 15 | 16:20:52 | 5 | C | alta | No | Correcta |  |
| 3 | 16 | 16:21:20 | 1 | A | alta | Sí – error de reconocimiento | Correcta |  |
| 3 | 17 | 16:21:35 | 10 | D | alta | No | Incorrecta |  |