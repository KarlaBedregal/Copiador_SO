// sha256.hpp - Implementacion simple de SHA-256 (FIPS 180-4).
// Se usa para verificar que el archivo copiado es identico al origen,
// sin depender de librerias externas como OpenSSL.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

class Sha256 {
public:
    Sha256();
    void actualizar(const void* datos, std::size_t n);  // agrega bytes al calculo
    std::string finalizar();                              // devuelve el hash en hexadecimal

    // Atajo: hash de un bloque completo en memoria
    static std::string de(const void* datos, std::size_t n);

private:
    void procesarBloque(const uint8_t bloque[64]);
    uint32_t h_[8];
    uint8_t buffer_[64];
    std::size_t usados_;   // bytes pendientes en buffer_
    uint64_t total_;       // total de bytes procesados
};
