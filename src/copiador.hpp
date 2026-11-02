// copiador.hpp - Utilidad de copia de archivos verificable y observable.
//
// Ruta de la copia:
//   origen --read()--> bufer (tamano de bloque configurable) --write()--> archivo temporal
//   --> fsync(temporal) --> verificar tamano y SHA-256 --> rename(temporal, destino) --> fsync(directorio)
//
// Si algo falla en cualquier punto, el temporal se borra y el destino anterior queda intacto.
#pragma once
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <sys/types.h>

namespace cp {

// Bandera global de cancelacion. La activa el manejador de SIGINT (Ctrl+C)
// o una prueba. El bucle de copia la revisa en cada bloque.
extern volatile std::sig_atomic_t g_cancelar;

// Instala el manejador de SIGINT/SIGTERM SIN SA_RESTART, para que una
// llamada read()/write() bloqueada se interrumpa con EINTR.
void instalarManejadorCancelacion();

enum class Resultado {
    OK = 0,
    ERROR_ARGUMENTOS,
    ERROR_ORIGEN,          // no se pudo abrir o leer el origen
    ERROR_DESTINO,         // no se pudo crear/escribir el temporal o renombrar
    SIN_ESPACIO,           // ENOSPC o EDQUOT (disco lleno o cuota agotada)
    CANCELADO,             // Ctrl+C o cancelacion simulada
    VERIFICACION_FALLIDA,  // tamano o hash distintos
    ORIGEN_CAMBIO          // el origen cambio de tamano durante la copia
};

const char* nombreResultado(Resultado r);

// Funciones de E/S intercambiables. Por defecto apuntan a ::read y ::write,
// pero las pruebas las reemplazan para simular escrituras parciales,
// EINTR, disco lleno, etc. sin necesitar hardware especial.
using FnLeer = std::function<ssize_t(int, void*, std::size_t)>;
using FnEscribir = std::function<ssize_t(int, const void*, std::size_t)>;

struct Opciones {
    std::size_t bloque = 64 * 1024;   // tamano del bufer de copia
    bool mostrarProgreso = true;      // reportar progreso en stderr
    int intervaloProgresoMs = 250;    // como maximo un reporte cada 250 ms
    bool verificar = true;            // calcular SHA-256 del origen, releer el temporal y comparar
    bool usarFsync = true;            // forzar persistencia antes de renombrar
    unsigned pausaPorBloqueMs = 0;    // solo para demostrar la cancelacion con Ctrl+C
    FnLeer leer;                      // vacio = ::read
    FnEscribir escribir;              // vacio = ::write
    std::FILE* salidaProgreso = stderr;
};

struct Estadisticas {
    Resultado resultado = Resultado::OK;
    int codigoErrno = 0;
    std::string mensaje;

    uint64_t tamanoOrigen = 0;
    uint64_t bytesCopiados = 0;
    uint64_t llamadasRead = 0;
    uint64_t llamadasWrite = 0;
    uint64_t lecturasCortas = 0;      // read() devolvio menos de lo pedido (sin ser EOF)
    uint64_t escriturasParciales = 0; // write() escribio menos de lo pedido
    uint64_t reintentosEintr = 0;     // llamadas interrumpidas y reintentadas
    uint64_t reportesProgreso = 0;

    double segCopia = 0, segFsync = 0, segVerificacion = 0, segTotal = 0;
    std::string shaOrigen, shaTemporal;
    std::string rutaTemporal;         // para auditoria: que temporal se uso
    bool destinoReemplazado = false;
};

// Copia 'origen' en 'destino'. Nunca deja un destino a medias:
// o el destino queda con la copia completa y verificada, o queda como estaba.
Estadisticas copiar(const std::string& origen, const std::string& destino,
                    const Opciones& op = Opciones());

// Lee un archivo completo y devuelve su SHA-256 (usado en verificacion y pruebas).
bool hashArchivo(const std::string& ruta, std::string& hex, std::size_t bloque = 64 * 1024);

}  // namespace cp
