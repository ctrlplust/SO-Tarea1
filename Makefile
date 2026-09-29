# Makefile del planificador de actividades

# Flags exactos de la pauta: gcc -Wall -Wextra -std=c17 -lpthread
# (-lpthread se incluye aunque no usemos hilos, para compilar igual que el corrector)
CC      = gcc
CFLAGS  = -Wall -Wextra -std=c17 -lpthread -O2
PROG    = planificador
SRC     = planificador.c

# Los planes de prueba viven en planes/ y los pasos en pasos/.
PLANES  = planes
PASOS   = pasos

# Se pueden cambiar al correr:  make run PLAN=planes/estres.txt K=10000
PLAN    ?= $(PLANES)/plan_ejemplo.txt
K       ?= 3

.PHONY: all run test test-pasos clean

all: $(PROG)

$(PROG): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

run: $(PROG)
	./$(PROG) $(PLAN) $(K)

# Las pruebas de siempre, en orden.
test: $(PROG)
	@echo "== plan valido con K=3 =="
	@./$(PROG) $(PLANES)/plan_ejemplo.txt 3 | tail -2
	@echo "== fallo de una actividad y cascada (K=2) =="
	@./$(PROG) $(PLANES)/falla.txt 2 | tail -2
	@echo "== plan con un ciclo (K=2) =="
	@./$(PROG) $(PLANES)/ciclo.txt 2 | tail -1
	@echo "== tiempos al azar (K=2) =="
	@./$(PROG) $(PLANES)/vacio.txt 2 | tail -2
	@echo "== lineas con menos de 4 campos (K=2) =="
	@./$(PROG) $(PLANES)/corto.txt 2 | tail -1
	@echo "== entradas invalidas (deben fallar con 1) =="
	@./$(PROG) $(PLANES)/malo1.txt 2; echo "   exit=$$?"
	@./$(PROG) $(PLANES)/malo2.txt 2; echo "   exit=$$?"
	@./$(PROG) $(PLANES)/malo3.txt 2; echo "   exit=$$?"
	@echo "== estres: 10000 actividades con K=10000 =="
	@./$(PROG) $(PLANES)/estres.txt 10000 | tail -2

# Cada paso tiene que compilar solo, con los flags de la pauta.
test-pasos:
	@for f in $(PASOS)/*.c; do \
	    $(CC) -Wall -Wextra -std=c17 -lpthread -o /dev/null $$f || exit 1; \
	    echo "ok $$f"; \
	done

clean:
	rm -f $(PROG) $(PASOS)/paso1 $(PASOS)/paso2 $(PASOS)/paso3 $(PASOS)/paso4 \
	      $(PASOS)/paso5a $(PASOS)/paso5b $(PASOS)/paso6 $(PASOS)/paso7 \
	      $(PASOS)/paso8 $(PASOS)/paso9 $(PASOS)/paso10
