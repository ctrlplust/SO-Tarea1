# Makefile del planificador de actividades

# -std=gnu17 y no c17: con c17 estricto desaparecen las declaraciones POSIX
# (fork, pipe, sigaction, nanosleep, sigprocmask).
CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu17 -O2
PROG    = planificador

# Se pueden cambiar al correr:  make run PLAN=estres.txt K=10000
PLAN    ?= plan_ejemplo.txt
K       ?= 3

.PHONY: all run test clean

all: $(PROG)

$(PROG): planificador.c
	$(CC) $(CFLAGS) -o $@ $<

run: $(PROG)
	./$(PROG) $(PLAN) $(K)

# Las pruebas de siempre, en orden.
test: $(PROG)
	@echo "== plan valido con K=3 =="
	@./$(PROG) plan_ejemplo.txt 3 | tail -2
	@echo "== fallo de una actividad y cascada (K=2) =="
	@./$(PROG) falla.txt 2 | tail -2
	@echo "== plan con un ciclo (K=2) =="
	@./$(PROG) ciclo.txt 2 | tail -1
	@echo "== tiempos al azar (K=2) =="
	@./$(PROG) vacio.txt 2 | tail -1
	@echo "== entradas invalidas (deben fallar con 1) =="
	@./$(PROG) malo1.txt 2; echo "   exit=$$?"
	@./$(PROG) malo2.txt 2; echo "   exit=$$?"
	@./$(PROG) malo3.txt 2; echo "   exit=$$?"
	@echo "== estres: 10000 actividades con K=10000 =="
	@./$(PROG) estres.txt 10000 | tail -2

clean:
	rm -f $(PROG) paso1 paso2 paso3 paso4 paso5a paso5b paso6 paso7 paso8 paso9 paso10
