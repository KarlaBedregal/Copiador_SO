// pruebas.cpp - Pruebas automatizadas del copiador.
// Compilar y ejecutar con:  make test
//
// Cada prueba trabaja en una carpeta temporal propia y revisa tres cosas:
// el resultado devuelto, el contenido del destino y que no queden temporales.
#include "copiador.hpp"
#include "sha256.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int g_total = 0, g_fallidas = 0;
std::string g_dir;
bool g_ok_actual = true;

#define REVISAR(cond)                                                                         \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cout << "      fallo: " #cond "  (linea " << __LINE__ << ")\n";              \
            g_ok_actual = false;                                                              \
        }                                                                                     \
    } while (0)

std::string ruta(const std::string& nombre) { return g_dir + "/" + nombre; }

void escribirArchivo(const std::string& r, const std::string& contenido) {
    FILE* f = std::fopen(r.c_str(), "wb");
    if (!contenido.empty()) std::fwrite(contenido.data(), 1, contenido.size(), f);
    std::fclose(f);
}

std::string leerArchivo(const std::string& r) {
    FILE* f = std::fopen(r.c_str(), "rb");
    if (!f) return "<NO EXISTE>";
    std::string s;
    char buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

bool existe(const std::string& r) { return ::access(r.c_str(), F_OK) == 0; }

std::string aleatorio(std::size_t n, unsigned semilla) {
    std::mt19937 gen(semilla);
    std::string s(n, '\0');
    for (auto& c : s) c = static_cast<char>(gen() & 0xFF);
    return s;
}

// Cuenta archivos temporales que hayan quedado en la carpeta de pruebas.
int contarTemporales() {
    int n = 0;
    DIR* d = ::opendir(g_dir.c_str());
    while (dirent* e = ::readdir(d)) {
        std::string nom = e->d_name;
        if (nom.size() > 1 && nom[0] == '.' && nom.find(".tmp.") != std::string::npos) ++n;
    }
    ::closedir(d);
    return n;
}

cp::Opciones opcionesSilenciosas(std::size_t bloque = 64 * 1024) {
    cp::Opciones op;
    op.bloque = bloque;
    op.mostrarProgreso = false;
    return op;
}

void ejecutar(const char* nombre, const std::function<void()>& prueba) {
    ++g_total;
    g_ok_actual = true;
    cp::g_cancelar = 0;
    std::cout << "[" << (g_total < 10 ? " " : "") << g_total << "] " << nombre << "\n";
    prueba();
    std::cout << "      " << (g_ok_actual ? "PASA" : "FALLA") << "\n";
    if (!g_ok_actual) ++g_fallidas;
}

}  // namespace

int main() {
    char plantilla[] = "/tmp/pruebas_copiador_XXXXXX";
    if (!::mkdtemp(plantilla)) { std::perror("mkdtemp"); return 1; }
    g_dir = plantilla;
    std::cout << "Carpeta de pruebas: " << g_dir << "\n\n";

    // ---------------------------------------------------------------------
    ejecutar("SHA-256 coincide con los vectores oficiales de prueba", [] {
        REVISAR(Sha256::de("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        REVISAR(Sha256::de("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        std::string m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        REVISAR(Sha256::de(m.data(), m.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    });

    // ---------------------------------------------------------------------
    ejecutar("Archivo vacio (0 bytes)", [] {
        escribirArchivo(ruta("vacio.bin"), "");
        auto est = cp::copiar(ruta("vacio.bin"), ruta("vacio_copia.bin"), opcionesSilenciosas());
        REVISAR(est.resultado == cp::Resultado::OK);
        REVISAR(existe(ruta("vacio_copia.bin")));
        REVISAR(leerArchivo(ruta("vacio_copia.bin")).empty());
        REVISAR(est.bytesCopiados == 0);
        REVISAR(contarTemporales() == 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("Archivo pequeno (menor que el bufer)", [] {
        std::string datos = "Hola, sistemas operativos. Archivo pequeno de prueba.\n";
        escribirArchivo(ruta("pequeno.txt"), datos);
        auto est = cp::copiar(ruta("pequeno.txt"), ruta("pequeno_copia.txt"), opcionesSilenciosas(64 * 1024));
        REVISAR(est.resultado == cp::Resultado::OK);
        REVISAR(leerArchivo(ruta("pequeno_copia.txt")) == datos);
        REVISAR(est.llamadasWrite == 1);  // todo cabe en un solo bloque
        REVISAR(contarTemporales() == 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("Archivo mayor que el bufer (1 MB + 123 bytes, bloque 4K)", [] {
        std::string datos = aleatorio(1024 * 1024 + 123, 1);
        escribirArchivo(ruta("grande.bin"), datos);
        auto est = cp::copiar(ruta("grande.bin"), ruta("grande_copia.bin"), opcionesSilenciosas(4096));
        REVISAR(est.resultado == cp::Resultado::OK);
        REVISAR(leerArchivo(ruta("grande_copia.bin")) == datos);
        REVISAR(est.llamadasWrite == 257);      // 256 bloques llenos + 1 de 123 bytes
        REVISAR(est.shaOrigen == est.shaTemporal);
        REVISAR(contarTemporales() == 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("Escrituras parciales simuladas (write acepta maximo 7 bytes)", [] {
        std::string datos = aleatorio(10000, 2);
        escribirArchivo(ruta("parcial.bin"), datos);
        auto op = opcionesSilenciosas(4096);
        op.escribir = [](int fd, const void* b, std::size_t n) { return ::write(fd, b, n > 7 ? 7 : n); };
        auto est = cp::copiar(ruta("parcial.bin"), ruta("parcial_copia.bin"), op);
        REVISAR(est.resultado == cp::Resultado::OK);
        REVISAR(leerArchivo(ruta("parcial_copia.bin")) == datos);
        REVISAR(est.escriturasParciales > 0);
        // Bloques de 4096, 4096 y 1808 bytes; cada uno necesita ceil(n/7) llamadas:
        // 586 + 586 + 259 = 1431
        REVISAR(est.llamadasWrite == 1431);
    });

    // ---------------------------------------------------------------------
    ejecutar("Lecturas parciales simuladas (read entrega maximo 1000 bytes)", [] {
        std::string datos = aleatorio(9500, 3);
        escribirArchivo(ruta("lectura.bin"), datos);
        auto op = opcionesSilenciosas(4096);
        op.leer = [](int fd, void* b, std::size_t n) { return ::read(fd, b, n > 1000 ? 1000 : n); };
        auto est = cp::copiar(ruta("lectura.bin"), ruta("lectura_copia.bin"), op);
        REVISAR(est.resultado == cp::Resultado::OK);
        REVISAR(leerArchivo(ruta("lectura_copia.bin")) == datos);
        REVISAR(est.lecturasCortas == 9);  // 10 lecturas con datos, la ultima no cuenta
    });

    // ---------------------------------------------------------------------
    ejecutar("Reintento ante EINTR (llamada interrumpida por una senal)", [] {
        std::string datos = aleatorio(50000, 4);
        escribirArchivo(ruta("eintr.bin"), datos);
        auto op = opcionesSilenciosas(4096);
        auto contador = std::make_shared<int>(0);
        op.escribir = [contador](int fd, const void* b, std::size_t n) -> ssize_t {
            if ((*contador)++ % 2 == 0) { errno = EINTR; return -1; }
            return ::write(fd, b, n);
        };
        auto est = cp::copiar(ruta("eintr.bin"), ruta("eintr_copia.bin"), op);
        REVISAR(est.resultado == cp::Resultado::OK);
        REVISAR(leerArchivo(ruta("eintr_copia.bin")) == datos);
        REVISAR(est.reintentosEintr > 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("Error simulado: disco lleno (ENOSPC) a mitad de la escritura", [] {
        std::string viejo = "CONTENIDO VALIDO ANTERIOR DEL DESTINO\n";
        escribirArchivo(ruta("lleno_destino.txt"), viejo);
        escribirArchivo(ruta("lleno_origen.bin"), aleatorio(200000, 5));
        auto op = opcionesSilenciosas(4096);
        auto escritos = std::make_shared<std::size_t>(0);
        op.escribir = [escritos](int fd, const void* b, std::size_t n) -> ssize_t {
            if (*escritos >= 100000) { errno = ENOSPC; return -1; }
            ssize_t w = ::write(fd, b, n);
            if (w > 0) *escritos += static_cast<std::size_t>(w);
            return w;
        };
        auto est = cp::copiar(ruta("lleno_origen.bin"), ruta("lleno_destino.txt"), op);
        REVISAR(est.resultado == cp::Resultado::SIN_ESPACIO);
        REVISAR(est.codigoErrno == ENOSPC);
        REVISAR(leerArchivo(ruta("lleno_destino.txt")) == viejo);  // el destino valido se conserva
        REVISAR(!est.rutaTemporal.empty() && !existe(est.rutaTemporal));
        REVISAR(contarTemporales() == 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("Error simulado: fallo de E/S (EIO) en la primera escritura", [] {
        escribirArchivo(ruta("eio.bin"), aleatorio(5000, 6));
        auto op = opcionesSilenciosas();
        op.escribir = [](int, const void*, std::size_t) -> ssize_t { errno = EIO; return -1; };
        auto est = cp::copiar(ruta("eio.bin"), ruta("eio_copia.bin"), op);
        REVISAR(est.resultado == cp::Resultado::ERROR_DESTINO);
        REVISAR(!existe(ruta("eio_copia.bin")));  // no se crea un destino a medias
        REVISAR(contarTemporales() == 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("Cancelacion a mitad de la copia (como Ctrl+C)", [] {
        std::string viejo = "version anterior que debe sobrevivir\n";
        escribirArchivo(ruta("cancel_destino.txt"), viejo);
        escribirArchivo(ruta("cancel_origen.bin"), aleatorio(300000, 7));
        auto op = opcionesSilenciosas(4096);
        auto escritos = std::make_shared<std::size_t>(0);
        op.escribir = [escritos](int fd, const void* b, std::size_t n) -> ssize_t {
            if (*escritos >= 120000) cp::g_cancelar = 1;  // llega la senal
            ssize_t w = ::write(fd, b, n);
            if (w > 0) *escritos += static_cast<std::size_t>(w);
            return w;
        };
        auto est = cp::copiar(ruta("cancel_origen.bin"), ruta("cancel_destino.txt"), op);
        REVISAR(est.resultado == cp::Resultado::CANCELADO);
        REVISAR(leerArchivo(ruta("cancel_destino.txt")) == viejo);
        REVISAR(contarTemporales() == 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("Corrupcion detectada: un byte alterado hace fallar el hash", [] {
        std::string viejo = "destino bueno\n";
        escribirArchivo(ruta("corrupto_destino.txt"), viejo);
        escribirArchivo(ruta("corrupto_origen.bin"), aleatorio(20000, 8));
        auto op = opcionesSilenciosas(4096);
        auto primero = std::make_shared<bool>(true);
        op.escribir = [primero](int fd, const void* b, std::size_t n) -> ssize_t {
            std::vector<char> copia(static_cast<const char*>(b), static_cast<const char*>(b) + n);
            if (*primero) { copia[0] ^= 0x01; *primero = false; }  // se voltea un bit
            return ::write(fd, copia.data(), n);
        };
        auto est = cp::copiar(ruta("corrupto_origen.bin"), ruta("corrupto_destino.txt"), op);
        REVISAR(est.resultado == cp::Resultado::VERIFICACION_FALLIDA);
        REVISAR(est.shaOrigen != est.shaTemporal);
        REVISAR(leerArchivo(ruta("corrupto_destino.txt")) == viejo);
        REVISAR(contarTemporales() == 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("Origen inexistente: error claro y sin crear el destino", [] {
        auto est = cp::copiar(ruta("no_existe.bin"), ruta("no_existe_copia.bin"), opcionesSilenciosas());
        REVISAR(est.resultado == cp::Resultado::ERROR_ORIGEN);
        REVISAR(est.codigoErrno == ENOENT);
        REVISAR(!existe(ruta("no_existe_copia.bin")));
    });

    // ---------------------------------------------------------------------
    ejecutar("Reemplazo de un destino existente solo al completar", [] {
        escribirArchivo(ruta("reemp_destino.txt"), "viejo");
        std::string nuevo = aleatorio(70000, 9);
        escribirArchivo(ruta("reemp_origen.bin"), nuevo);
        auto est = cp::copiar(ruta("reemp_origen.bin"), ruta("reemp_destino.txt"), opcionesSilenciosas(8192));
        REVISAR(est.resultado == cp::Resultado::OK);
        REVISAR(est.destinoReemplazado);
        REVISAR(leerArchivo(ruta("reemp_destino.txt")) == nuevo);
    });

    // ---------------------------------------------------------------------
    ejecutar("Caso extremo: bloque de 1 byte y bloque 0 invalido", [] {
        std::string datos = "abcdefghij";
        escribirArchivo(ruta("uno.txt"), datos);
        auto est = cp::copiar(ruta("uno.txt"), ruta("uno_copia.txt"), opcionesSilenciosas(1));
        REVISAR(est.resultado == cp::Resultado::OK);
        REVISAR(est.llamadasRead == 11);  // 10 bytes + la lectura que detecta EOF
        REVISAR(leerArchivo(ruta("uno_copia.txt")) == datos);
        auto est0 = cp::copiar(ruta("uno.txt"), ruta("uno_copia0.txt"), opcionesSilenciosas(0));
        REVISAR(est0.resultado == cp::Resultado::ERROR_ARGUMENTOS);
        REVISAR(contarTemporales() == 0);
    });

    // ---------------------------------------------------------------------
    ejecutar("El progreso no satura la salida (maximo 1 reporte cada 250 ms)", [] {
        escribirArchivo(ruta("progreso.bin"), aleatorio(4 * 1024 * 1024, 10));
        auto op = opcionesSilenciosas(1024);
        op.mostrarProgreso = true;
        op.intervaloProgresoMs = 250;
        FILE* sumidero = std::tmpfile();
        op.salidaProgreso = sumidero;
        auto est = cp::copiar(ruta("progreso.bin"), ruta("progreso_copia.bin"), op);
        std::fclose(sumidero);
        REVISAR(est.resultado == cp::Resultado::OK);
        // 4096 bloques copiados, pero los reportes dependen del tiempo, no de los bloques
        uint64_t maximo = static_cast<uint64_t>(est.segCopia / 0.25) + 2;
        std::cout << "      bloques=" << est.llamadasWrite << " reportes=" << est.reportesProgreso
                  << " (maximo permitido " << maximo << ")\n";
        REVISAR(est.reportesProgreso <= maximo);
        REVISAR(est.reportesProgreso < est.llamadasWrite);
    });

    // Limpieza de la carpeta de pruebas
    std::string cmd = "rm -rf '" + g_dir + "'";
    if (std::system(cmd.c_str()) != 0) std::cout << "(no se pudo borrar " << g_dir << ")\n";

    std::cout << "\nResultado: " << (g_total - g_fallidas) << " de " << g_total << " pruebas pasaron\n";
    return g_fallidas == 0 ? 0 : 1;
}
