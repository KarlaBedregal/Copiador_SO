# Makefile - Copiador verificable y observable (Sistemas Operativos, Semana 9)
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
NUCLEO    = src/copiador.cpp src/sha256.cpp
CABECERAS = src/copiador.hpp src/sha256.hpp

all: bin/copiador bin/pruebas

bin/copiador: src/main.cpp $(NUCLEO) $(CABECERAS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp $(NUCLEO)

bin/pruebas: tests/pruebas.cpp $(NUCLEO) $(CABECERAS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tests/pruebas.cpp $(NUCLEO)

# Ejecuta las pruebas automatizadas
test: bin/pruebas
	./bin/pruebas

# Mide tiempos con tres tamanos de bloque (guarda resultados/medicion.csv)
medir: bin/copiador
	./medir.sh

clean:
	rm -rf bin resultados

.PHONY: all test medir clean
