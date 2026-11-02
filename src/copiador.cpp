// copiador.cpp - Implementacion de la copia robusta.
#include "copiador.hpp"
#include "sha256.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cp {

volatile std::sig_atomic_t g_cancelar = 0;

namespace {

using Reloj = std::chrono::steady_clock;

double segundosDesde(Reloj::time_point t0) {
    return std::chrono::duration<double>(Reloj::now() - t0).count();
}

void manejadorSenal(int) { g_cancelar = 1; }

std::string errnoTexto(int e) { return std::string(std::strerror(e)) + " (errno " + std::to_string(e) + ")"; }

// Separa "carpeta/archivo" en carpeta y nombre. Sin '/', la carpeta es ".".
void separarRuta(const std::string& ruta, std::string& dir, std::string& base) {
    std::size_t pos = ruta.find_last_of('/');
    if (pos == std::string::npos) { dir = "."; base = ruta; }
    else if (pos == 0)            { dir = "/"; base = ruta.substr(1); }
    else                          { dir = ruta.substr(0, pos); base = ruta.substr(pos + 1); }
}

// Guardian RAII: cierra descriptores y, si la copia no se confirmo,
// borra el archivo temporal. Asi ningun camino de error deja basura.
struct Guardian {
    int fdOrigen = -1;
    int fdTemporal = -1;
    std::string rutaTemporal;
    bool confirmado = false;

    ~Guardian() {
        if (fdOrigen >= 0) ::close(fdOrigen);
        if (fdTemporal >= 0) ::close(fdTemporal);
        if (!confirmado && !rutaTemporal.empty()) ::unlink(rutaTemporal.c_str());
    }
};

// Reporte de progreso con limite de frecuencia para no saturar la salida.
class Progreso {
public:
    Progreso(const Opciones& op, uint64_t total, Estadisticas& est)
        : op_(op), total_(total), est_(est), inicio_(Reloj::now()), ultimo_(inicio_) {
        esTerminal_ = op_.salidaProgreso && ::isatty(::fileno(op_.salidaProgreso));
    }

    void tal_vez(uint64_t hecho) {
        if (!op_.mostrarProgreso || !op_.salidaProgreso) return;
        auto ahora = Reloj::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(ahora - ultimo_).count();
        if (ms < op_.intervaloProgresoMs) return;   // muy pronto: no se imprime
        ultimo_ = ahora;
        imprimir(hecho, false);
    }

    void final(uint64_t hecho) {
        if (!op_.mostrarProgreso || !op_.salidaProgreso) return;
        imprimir(hecho, true);
    }

private:
    void imprimir(uint64_t hecho, bool esFinal) {
        double seg = segundosDesde(inicio_);
        double mb = hecho / (1024.0 * 1024.0);
        double velocidad = seg > 0 ? mb / seg : 0;
        double pct = total_ > 0 ? 100.0 * hecho / total_ : 100.0;
        int llenos = static_cast<int>(pct / 5);
        char barra[21];
        for (int i = 0; i < 20; ++i) barra[i] = i < llenos ? '#' : '-';
        barra[20] = '\0';
        std::fprintf(op_.salidaProgreso, "%s[%s] %5.1f%%  %8.2f / %.2f MB  %7.1f MB/s%s",
                     esTerminal_ ? "\r" : "", barra, pct, mb, total_ / (1024.0 * 1024.0), velocidad,
                     (esFinal || !esTerminal_) ? "\n" : "");
        std::fflush(op_.salidaProgreso);
        est_.reportesProgreso++;
    }

    const Opciones& op_;
    uint64_t total_;
    Estadisticas& est_;
    Reloj::time_point inicio_, ultimo_;
    bool esTerminal_ = false;
};

Estadisticas& fallar(Estadisticas& est, Resultado r, int e, const std::string& msg) {
    est.resultado = r;
    est.codigoErrno = e;
    est.mensaje = e ? msg + ": " + errnoTexto(e) : msg;
    return est;
}

Resultado clasificarErrorEscritura(int e) {
    if (e == ENOSPC || e == EDQUOT) return Resultado::SIN_ESPACIO;
    return Resultado::ERROR_DESTINO;
}

}  // namespace

const char* nombreResultado(Resultado r) {
    switch (r) {
        case Resultado::OK: return "OK";
        case Resultado::ERROR_ARGUMENTOS: return "ERROR_ARGUMENTOS";
        case Resultado::ERROR_ORIGEN: return "ERROR_ORIGEN";
        case Resultado::ERROR_DESTINO: return "ERROR_DESTINO";
        case Resultado::SIN_ESPACIO: return "SIN_ESPACIO";
        case Resultado::CANCELADO: return "CANCELADO";
        case Resultado::VERIFICACION_FALLIDA: return "VERIFICACION_FALLIDA";
        case Resultado::ORIGEN_CAMBIO: return "ORIGEN_CAMBIO";
    }
    return "?";
}

void instalarManejadorCancelacion() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejadorSenal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // sin SA_RESTART: las llamadas bloqueadas regresan con EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

bool hashArchivo(const std::string& ruta, std::string& hex, std::size_t bloque) {
    int fd = ::open(ruta.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::vector<char> buf(bloque);
    Sha256 sha;
    while (true) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno; ::close(fd); errno = e;
            return false;
        }
        if (n == 0) break;
        sha.actualizar(buf.data(), static_cast<std::size_t>(n));
    }
    ::close(fd);
    hex = sha.finalizar();
    return true;
}

Estadisticas copiar(const std::string& origen, const std::string& destino, const Opciones& op) {
    Estadisticas est;
    auto t0 = Reloj::now();

    if (op.bloque == 0) return fallar(est, Resultado::ERROR_ARGUMENTOS, 0, "El tamano de bloque debe ser mayor que 0");

    FnLeer leer = op.leer ? op.leer : FnLeer([](int fd, void* b, std::size_t n) { return ::read(fd, b, n); });
    FnEscribir escribir = op.escribir ? op.escribir
                                      : FnEscribir([](int fd, const void* b, std::size_t n) { return ::write(fd, b, n); });

    Guardian g;

    // 1) Abrir el origen y conocer su tamano y permisos.
    g.fdOrigen = ::open(origen.c_str(), O_RDONLY | O_CLOEXEC);
    if (g.fdOrigen < 0) return fallar(est, Resultado::ERROR_ORIGEN, errno, "No se pudo abrir el origen '" + origen + "'");
    struct stat stOrigen;
    if (::fstat(g.fdOrigen, &stOrigen) != 0) return fallar(est, Resultado::ERROR_ORIGEN, errno, "fstat del origen");
    if (S_ISDIR(stOrigen.st_mode)) return fallar(est, Resultado::ERROR_ORIGEN, EISDIR, "El origen es un directorio");
    const bool esRegular = S_ISREG(stOrigen.st_mode);
    est.tamanoOrigen = esRegular ? static_cast<uint64_t>(stOrigen.st_size) : 0;

    // El destino no puede ser un directorio ni el mismo archivo que el origen.
    struct stat stDestino;
    if (::stat(destino.c_str(), &stDestino) == 0) {
        if (S_ISDIR(stDestino.st_mode))
            return fallar(est, Resultado::ERROR_ARGUMENTOS, EISDIR, "El destino es un directorio");
        if (stDestino.st_dev == stOrigen.st_dev && stDestino.st_ino == stOrigen.st_ino)
            return fallar(est, Resultado::ERROR_ARGUMENTOS, 0, "Origen y destino son el mismo archivo");
    }

    // 2) Crear el temporal en la MISMA carpeta del destino. Esto es necesario
    //    para que rename() sea atomico (rename no funciona entre sistemas de archivos).
    std::string dir, base;
    separarRuta(destino, dir, base);
    std::string plantilla = dir + "/." + base + ".tmp.XXXXXX";
    std::vector<char> nombre(plantilla.begin(), plantilla.end());
    nombre.push_back('\0');
    g.fdTemporal = ::mkstemp(nombre.data());
    if (g.fdTemporal < 0) return fallar(est, clasificarErrorEscritura(errno), errno, "No se pudo crear el temporal en '" + dir + "'");
    g.rutaTemporal = nombre.data();
    est.rutaTemporal = g.rutaTemporal;
    ::fchmod(g.fdTemporal, stOrigen.st_mode & 07777);  // mismos permisos que el origen

    // 3) Bucle de copia por bloques.
    std::vector<char> buf;
    try {
        buf.resize(op.bloque);
    } catch (const std::bad_alloc&) {
        return fallar(est, Resultado::ERROR_ARGUMENTOS, ENOMEM, "No hay memoria para un bufer de ese tamano");
    }
    Sha256 shaOrigen;
    Progreso progreso(op, est.tamanoOrigen, est);
    bool anteriorFueCorta = false;
    auto tCopia = Reloj::now();

    while (true) {
        if (g_cancelar) return fallar(est, Resultado::CANCELADO, 0, "Copia cancelada por el usuario");

        ssize_t n = leer(g.fdOrigen, buf.data(), buf.size());
        est.llamadasRead++;
        if (n < 0) {
            if (errno == EINTR) { est.reintentosEintr++; continue; }  // se revisa cancelacion arriba
            return fallar(est, Resultado::ERROR_ORIGEN, errno, "Error leyendo el origen");
        }
        if (n == 0) break;  // EOF

        // Una lectura corta seguida de mas datos es una lectura parcial real
        // (la ultima porcion del archivo siempre es corta y no cuenta).
        if (anteriorFueCorta) est.lecturasCortas++;
        anteriorFueCorta = static_cast<std::size_t>(n) < buf.size();

        if (op.verificar) shaOrigen.actualizar(buf.data(), static_cast<std::size_t>(n));

        // Escribir TODO lo leido, aunque write() acepte solo una parte cada vez.
        // 'p' avanza dentro del mismo bufer: no se copian los bytes restantes
        // a otro lado (equivalente al memoryview de Python).
        const char* p = buf.data();
        std::size_t pendiente = static_cast<std::size_t>(n);
        while (pendiente > 0) {
            if (g_cancelar) return fallar(est, Resultado::CANCELADO, 0, "Copia cancelada por el usuario");
            ssize_t w = escribir(g.fdTemporal, p, pendiente);
            est.llamadasWrite++;
            if (w < 0) {
                if (errno == EINTR) { est.reintentosEintr++; continue; }
                int e = errno;
                return fallar(est, clasificarErrorEscritura(e), e,
                              "Error escribiendo el temporal tras " + std::to_string(est.bytesCopiados) + " bytes");
            }
            if (w == 0)  // nunca deberia pasar con archivos; se evita un bucle infinito
                return fallar(est, Resultado::ERROR_DESTINO, EIO, "write() devolvio 0 bytes");
            if (static_cast<std::size_t>(w) < pendiente) est.escriturasParciales++;
            p += w;
            pendiente -= static_cast<std::size_t>(w);
            est.bytesCopiados += static_cast<uint64_t>(w);
        }

        progreso.tal_vez(est.bytesCopiados);
        if (op.pausaPorBloqueMs) std::this_thread::sleep_for(std::chrono::milliseconds(op.pausaPorBloqueMs));
    }
    est.segCopia = segundosDesde(tCopia);
    progreso.final(est.bytesCopiados);
    if (op.verificar) est.shaOrigen = shaOrigen.finalizar();

    // El origen pudo cambiar mientras se copiaba.
    if (esRegular) {
        struct stat stFin;
        if (::fstat(g.fdOrigen, &stFin) == 0 &&
            (static_cast<uint64_t>(stFin.st_size) != est.bytesCopiados || stFin.st_mtim.tv_sec != stOrigen.st_mtim.tv_sec ||
             stFin.st_mtim.tv_nsec != stOrigen.st_mtim.tv_nsec))
            return fallar(est, Resultado::ORIGEN_CAMBIO, 0,
                          "El origen cambio durante la copia (" + std::to_string(est.bytesCopiados) + " bytes leidos, " +
                              std::to_string(stFin.st_size) + " actuales)");
    }

    // 4) Pedir persistencia: write() exitoso solo significa que los datos
    //    estan en la page cache; fsync() los obliga a llegar al dispositivo.
    auto tFsync = Reloj::now();
    if (op.usarFsync && ::fsync(g.fdTemporal) != 0) {
        int e = errno;
        return fallar(est, clasificarErrorEscritura(e), e, "fsync del temporal fallo");
    }
    est.segFsync = segundosDesde(tFsync);
    int fdTmp = g.fdTemporal;
    g.fdTemporal = -1;
    if (::close(fdTmp) != 0) {
        int e = errno;
        return fallar(est, clasificarErrorEscritura(e), e, "close del temporal fallo");
    }

    // 5) Verificar tamano y hash del temporal antes de reemplazar el destino.
    auto tVerif = Reloj::now();
    struct stat stTmp;
    if (::stat(g.rutaTemporal.c_str(), &stTmp) != 0)
        return fallar(est, Resultado::ERROR_DESTINO, errno, "stat del temporal");
    if (static_cast<uint64_t>(stTmp.st_size) != est.bytesCopiados)
        return fallar(est, Resultado::VERIFICACION_FALLIDA, 0,
                      "Tamano distinto: temporal=" + std::to_string(stTmp.st_size) + " esperado=" + std::to_string(est.bytesCopiados));
    if (op.verificar) {
        if (!hashArchivo(g.rutaTemporal, est.shaTemporal, op.bloque))
            return fallar(est, Resultado::ERROR_DESTINO, errno, "No se pudo releer el temporal");
        if (est.shaTemporal != est.shaOrigen)
            return fallar(est, Resultado::VERIFICACION_FALLIDA, 0, "El SHA-256 del temporal no coincide con el del origen");
    }
    est.segVerificacion = segundosDesde(tVerif);

    // 6) Reemplazo atomico: rename() cambia la entrada del directorio de una vez.
    //    Cualquier lector ve el archivo viejo completo o el nuevo completo, nunca uno a medias.
    if (::rename(g.rutaTemporal.c_str(), destino.c_str()) != 0)
        return fallar(est, Resultado::ERROR_DESTINO, errno, "rename del temporal al destino fallo");
    g.confirmado = true;
    est.destinoReemplazado = true;

    // 7) fsync de la carpeta para que el cambio de nombre tambien sea durable.
    if (op.usarFsync) {
        int fdDir = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fdDir >= 0) { ::fsync(fdDir); ::close(fdDir); }
    }

    est.segTotal = segundosDesde(t0);
    est.resultado = Resultado::OK;
    est.mensaje = "Copia completada y verificada";
    return est;
}

}  // namespace cp
