# Copiador verificable y observable (C++)

**Sistemas Operativos · UNSA · 2026-B · Semana 9: Sistemas de Almacenamiento y E/S**
Docente: MSc. Norman Patrick Harvey Arce · Alumna: Bedregal Coaguila, Karla Miluska

Utilidad de copia de archivos con tamaño de bloque configurable, escrita en C++17 con llamadas POSIX
(`open`, `read`, `write`, `fsync`, `rename`). Nunca deja un destino a medias: o el destino queda con la
copia completa y verificada, o queda exactamente como estaba.

## Ruta de la copia

```
origen --read()--> búfer (bloque) --write()--> .destino.tmp.XXXXXX
       --> fsync(temporal) --> verificar tamaño y SHA-256
       --> rename(temporal, destino) --> fsync(carpeta)
```

## Requisitos cubiertos

| Requisito | Cómo se cumple |
|---|---|
| Lecturas y escrituras parciales | bucle que avanza un puntero dentro del búfer hasta escribir todo lo leído |
| Copiar a un archivo temporal | `mkstemp()` en la misma carpeta del destino |
| Progreso sin saturar la salida | como máximo un reporte cada 250 ms, en `stderr` |
| Falta de espacio y cancelación | `ENOSPC`/`EDQUOT` → `SIN_ESPACIO`; Ctrl+C (`SIGINT`) → `CANCELADO` |
| Verificar tamaño y hash | `fstat` + SHA-256 del origen comparado con el del temporal releído |
| Reemplazar solo al completar | `rename()` atómico después de `fsync` y verificación |
| Limpiar temporales ante error | objeto RAII que borra el temporal en cualquier salida con error |

## Compilar y ejecutar (Linux o WSL)

```bash
make                                   # compila bin/copiador y bin/pruebas
./bin/copiador -b 64K origen destino   # copia con bloque de 64 KB
./bin/copiador --estadisticas origen destino
make test                              # 15 pruebas automatizadas
./medir.sh 128 3                       # medición con bloques 4K, 64K y 1M
```

### Opciones

```
-b, --bloque N            tamaño de bloque (4K, 64K, 1M...). Defecto 64K
-q, --silencio            sin barra de progreso
--sin-verificar           no calcular ni comparar SHA-256
--sin-fsync               no forzar persistencia
--estadisticas            contadores de llamadas, parciales y tiempos
--csv                     una línea CSV (la usa medir.sh)
--simular-parcial K       write() escribe como máximo K bytes
--simular-lectura-parcial K  read() lee como máximo K bytes
--simular-sin-espacio N   ENOSPC al llegar a N bytes
--simular-eintr           EINTR en 1 de cada 3 write()
--cancelar-en N           cancelar al llegar a N bytes
--lento MS                pausa por bloque (para probar Ctrl+C a mano)
```

### Códigos de salida

`0` OK · `1` argumentos · `2` error en origen · `3` error en destino · `4` sin espacio ·
`5` cancelado · `6` verificación fallida · `7` el origen cambió durante la copia

## Estructura

```
src/sha256.hpp, src/sha256.cpp      SHA-256 propio (sin dependencias)
src/copiador.hpp, src/copiador.cpp  núcleo: copia robusta y estadísticas
src/main.cpp                        programa de línea de comandos
tests/pruebas.cpp                   15 pruebas automatizadas
medir.sh                            medición con tres tamaños de bloque
Makefile
```
