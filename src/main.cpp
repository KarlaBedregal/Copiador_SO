// main.cpp - Programa de linea de comandos del copiador.
//
// Uso: ./copiador [opciones] ORIGEN DESTINO
//
// Opciones normales:
//   -b, --bloque N       tamano de bloque (acepta sufijos K y M, ej: 4K, 64K, 1M). Defecto 64K
//   -q, --silencio       no mostrar progreso
//   --sin-verificar      no releer el temporal para comparar el hash
//   --sin-fsync          no forzar persistencia (para medir su costo)
//   --estadisticas       mostrar contadores de llamadas, parciales y tiempos
//   --csv                imprimir una linea CSV con los resultados (para medir.sh)
//
// Opciones de simulacion (para demostrar el manejo de fallos):
//   --simular-parcial K           cada write() escribe como maximo K bytes
//   --simular-lectura-parcial K   cada read() lee como maximo K bytes
//   --simular-sin-espacio N       write() falla con ENOSPC al llegar a N bytes escritos
//   --simular-eintr               1 de cada 3 llamadas a write() devuelve EINTR
//   --cancelar-en N               se cancela sola al llegar a N bytes (como un Ctrl+C)
//   --lento MS                    espera MS milisegundos por bloque (para probar Ctrl+C a mano)
#include "copiador.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

bool leerTamano(const std::string& s, std::size_t& out) {
    if (s.empty()) return false;
    char* fin = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s.c_str(), &fin, 10);
    if (errno != 0 || fin == s.c_str()) return false;
    std::string suf(fin);
    if (suf == "" || suf == "B") {}
    else if (suf == "K" || suf == "k" || suf == "KB") v *= 1024ULL;
    else if (suf == "M" || suf == "m" || suf == "MB") v *= 1024ULL * 1024ULL;
    else return false;
    out = static_cast<std::size_t>(v);
    return true;
}

void ayuda(const char* prog) {
    std::cerr << "Uso: " << prog << " [opciones] ORIGEN DESTINO\n"
              << "  -b, --bloque N            tamano de bloque (4K, 64K, 1M...). Defecto 64K\n"
              << "  -q, --silencio            sin barra de progreso\n"
              << "  --sin-verificar           no comparar SHA-256 al final\n"
              << "  --sin-fsync               no forzar persistencia\n"
              << "  --estadisticas            mostrar contadores y tiempos\n"
              << "  --csv                     una linea CSV con resultados\n"
              << "  --simular-parcial K       write() escribe como maximo K bytes\n"
              << "  --simular-lectura-parcial K  read() lee como maximo K bytes\n"
              << "  --simular-sin-espacio N   ENOSPC al llegar a N bytes\n"
              << "  --simular-eintr           EINTR en 1 de cada 3 write()\n"
              << "  --cancelar-en N           cancelar al llegar a N bytes\n"
              << "  --lento MS                pausa por bloque (para probar Ctrl+C)\n";
}

int codigoSalida(cp::Resultado r) { return static_cast<int>(r); }

}  // namespace

int main(int argc, char** argv) {
    cp::Opciones op;
    bool estadisticas = false, csv = false;
    std::size_t simParcial = 0, simLecturaParcial = 0, simSinEspacio = 0, cancelarEn = 0;
    bool simEintr = false, haySinEspacio = false, hayCancelar = false;
    std::string origen, destino;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto siguiente = [&](std::size_t& dst) -> bool {
            if (i + 1 >= argc || !leerTamano(argv[i + 1], dst)) {
                std::cerr << "Valor invalido para " << a << "\n";
                return false;
            }
            ++i;
            return true;
        };
        if (a == "-h" || a == "--ayuda" || a == "--help") { ayuda(argv[0]); return 0; }
        else if (a == "-b" || a == "--bloque") { if (!siguiente(op.bloque)) return 1; }
        else if (a == "-q" || a == "--silencio") op.mostrarProgreso = false;
        else if (a == "--sin-verificar") op.verificar = false;
        else if (a == "--sin-fsync") op.usarFsync = false;
        else if (a == "--estadisticas") estadisticas = true;
        else if (a == "--csv") { csv = true; op.mostrarProgreso = false; }
        else if (a == "--simular-parcial") { if (!siguiente(simParcial)) return 1; }
        else if (a == "--simular-lectura-parcial") { if (!siguiente(simLecturaParcial)) return 1; }
        else if (a == "--simular-sin-espacio") { if (!siguiente(simSinEspacio)) return 1; haySinEspacio = true; }
        else if (a == "--simular-eintr") simEintr = true;
        else if (a == "--cancelar-en") { if (!siguiente(cancelarEn)) return 1; hayCancelar = true; }
        else if (a == "--lento") { std::size_t ms; if (!siguiente(ms)) return 1; op.pausaPorBloqueMs = static_cast<unsigned>(ms); }
        else if (!a.empty() && a[0] == '-') { std::cerr << "Opcion desconocida: " << a << "\n"; ayuda(argv[0]); return 1; }
        else if (origen.empty()) origen = a;
        else if (destino.empty()) destino = a;
        else { std::cerr << "Demasiados argumentos\n"; return 1; }
    }
    if (origen.empty() || destino.empty()) { ayuda(argv[0]); return 1; }

    // Si se pidio alguna simulacion, se envuelve write()/read() reales con la falla.
    auto escritos = std::make_shared<std::size_t>(0);
    auto contador = std::make_shared<unsigned>(0);
    if (simParcial || haySinEspacio || simEintr || hayCancelar) {
        op.escribir = [=](int fd, const void* b, std::size_t n) -> ssize_t {
            if (simEintr && ((*contador)++ % 3 == 1)) { errno = EINTR; return -1; }
            if (hayCancelar && *escritos >= cancelarEn) cp::g_cancelar = 1;  // como si llegara Ctrl+C
            if (haySinEspacio && *escritos >= simSinEspacio) { errno = ENOSPC; return -1; }
            std::size_t pedir = n;
            if (simParcial && pedir > simParcial) pedir = simParcial;
            if (haySinEspacio && *escritos + pedir > simSinEspacio) pedir = simSinEspacio - *escritos;
            ssize_t w = ::write(fd, b, pedir);
            if (w > 0) *escritos += static_cast<std::size_t>(w);
            return w;
        };
    }
    if (simLecturaParcial) {
        op.leer = [=](int fd, void* b, std::size_t n) -> ssize_t {
            return ::read(fd, b, n > simLecturaParcial ? simLecturaParcial : n);
        };
    }

    cp::instalarManejadorCancelacion();
    auto t0 = std::chrono::steady_clock::now();
    cp::Estadisticas est = cp::copiar(origen, destino, op);
    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (csv) {
        // bloque,bytes,seg_copia,seg_fsync,seg_verificacion,seg_total,MBps_copia,llamadas_read,llamadas_write,resultado
        double mbps = est.segCopia > 0 ? est.bytesCopiados / (1024.0 * 1024.0) / est.segCopia : 0;
        std::cout << op.bloque << "," << est.bytesCopiados << "," << est.segCopia << "," << est.segFsync << ","
                  << est.segVerificacion << "," << total << "," << mbps << "," << est.llamadasRead << ","
                  << est.llamadasWrite << "," << cp::nombreResultado(est.resultado) << "\n";
        return codigoSalida(est.resultado);
    }

    if (est.resultado == cp::Resultado::OK) {
        std::cout << "OK: " << est.mensaje << "\n"
                  << "  bytes:      " << est.bytesCopiados << "\n"
                  << "  SHA-256:    " << est.shaOrigen << "\n";
        if (op.verificar) std::cout << "  verificado: SHA-256 del destino coincide\n";
    } else {
        std::cerr << "FALLO [" << cp::nombreResultado(est.resultado) << "]: " << est.mensaje << "\n"
                  << "  bytes escritos en el temporal antes del fallo: " << est.bytesCopiados << " de "
                  << est.tamanoOrigen << "\n"
                  << "  temporal eliminado: "
                  << (est.rutaTemporal.empty() ? "(no se llego a crear)"
                                               : (::access(est.rutaTemporal.c_str(), F_OK) != 0 ? "si" : "NO"))
                  << "\n"
                  << "  destino original intacto: " << (est.destinoReemplazado ? "no" : "si") << "\n";
    }

    if (estadisticas) {
        std::cout << "Estadisticas:\n"
                  << "  tamano de bloque:       " << op.bloque << " bytes\n"
                  << "  llamadas read():        " << est.llamadasRead << "\n"
                  << "  llamadas write():       " << est.llamadasWrite << "\n"
                  << "  lecturas cortas:        " << est.lecturasCortas << "\n"
                  << "  escrituras parciales:   " << est.escriturasParciales << "\n"
                  << "  reintentos por EINTR:   " << est.reintentosEintr << "\n"
                  << "  reportes de progreso:   " << est.reportesProgreso << "\n"
                  << "  tiempo copia:           " << est.segCopia << " s\n"
                  << "  tiempo fsync:           " << est.segFsync << " s\n"
                  << "  tiempo verificacion:    " << est.segVerificacion << " s\n"
                  << "  tiempo total:           " << total << " s\n";
    }
    return codigoSalida(est.resultado);
}
