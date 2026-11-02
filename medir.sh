#!/usr/bin/env bash
# medir.sh - Mide el copiador con tres tamanos de bloque.
#
# Uso:   ./medir.sh [TAMANO_MB] [REPETICIONES]
#        (por defecto 128 MB y 3 repeticiones)
#
# Para cada bloque (4K, 64K, 1M) copia el mismo archivo varias veces y guarda
# en resultados/medicion.csv: tiempo de copia, tiempo de fsync, tiempo de
# verificacion, velocidad y cantidad de llamadas read()/write().
# Tambien repite el bloque de 64K sin fsync para ver cuanto cuesta la persistencia,
# y mide la copia sola (sin hash ni fsync) para ver el efecto del tamano de bloque.
set -euo pipefail

TAM_MB=${1:-128}
REPS=${2:-3}
BIN=./bin/copiador
DIR=resultados
ORIGEN=$DIR/origen_${TAM_MB}MB.bin
DESTINO=$DIR/destino.bin
CSV=$DIR/medicion.csv

[ -x "$BIN" ] || { echo "Primero compila con: make"; exit 1; }
mkdir -p "$DIR"

if [ ! -f "$ORIGEN" ]; then
    echo "Creando archivo de prueba de ${TAM_MB} MB con datos aleatorios..."
    head -c "$((TAM_MB * 1024 * 1024))" /dev/urandom > "$ORIGEN"
fi

# Intenta vaciar la page cache (solo funciona con permisos de administrador).
vaciar_cache() {
    sync
    if [ "$(id -u)" -eq 0 ]; then
        echo 3 > /proc/sys/vm/drop_caches 2>/dev/null && return 0
    elif sudo -n true 2>/dev/null; then
        echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null && return 0
    fi
    return 1
}

echo "modo,bloque,bytes,seg_copia,seg_fsync,seg_verificacion,seg_total,MBps_copia,llamadas_read,llamadas_write,resultado" > "$CSV"

medir() {  # $1 = modo (con_fsync | sin_fsync | cache_fria), $2 = bloque, resto = opciones extra
    local modo=$1 bloque=$2; shift 2
    local linea
    linea=$("$BIN" --csv -b "$bloque" "$@" "$ORIGEN" "$DESTINO")
    echo "$modo,$linea" >> "$CSV"
}

echo "Midiendo: archivo de ${TAM_MB} MB, ${REPS} repeticiones por caso"
for bloque in 4K 64K 1M; do
    for _ in $(seq "$REPS"); do medir con_fsync "$bloque"; done
done
for _ in $(seq "$REPS"); do medir sin_fsync 64K --sin-fsync; done
# Solo copia: sin hash ni fsync, para ver el costo puro de read()/write() segun el bloque
for bloque in 4K 64K 1M; do
    for _ in $(seq "$REPS"); do medir solo_copia "$bloque" --sin-verificar --sin-fsync; done
done

if vaciar_cache; then
    for bloque in 4K 64K 1M; do
        vaciar_cache; medir cache_fria "$bloque"
    done
    echo "(se midio tambien con la page cache vaciada)"
else
    echo "(sin permisos para vaciar la page cache: se omite la medicion en frio)"
fi
rm -f "$DESTINO"

echo
echo "Promedios por caso (tiempos en segundos):"
awk -F, 'NR>1 {
    k=$1" "$2; n[k]++; c[k]+=$4; f[k]+=$5; v[k]+=$6; t[k]+=$7; mb[k]+=$8; r[k]=$9; w[k]=$10
    if (!(k in orden)) { orden[k]=++m; claves[m]=k }
} END {
    printf "%-20s %10s %10s %10s %10s %10s %10s %10s\n", "caso", "copia", "fsync", "verif", "total", "MB/s", "read()", "write()"
    for (i=1;i<=m;i++) { k=claves[i]
        printf "%-20s %10.4f %10.4f %10.4f %10.4f %10.1f %10d %10d\n", k, c[k]/n[k], f[k]/n[k], v[k]/n[k], t[k]/n[k], mb[k]/n[k], r[k], w[k]
    }
}' "$CSV"
echo
echo "Resultados detallados en $CSV"
