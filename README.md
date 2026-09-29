# Planificador de actividades con procesos y pipes

Programa en C que ejecuta un plan de actividades usando procesos del sistema
operativo: cada actividad es un hijo creado con `fork()`, y los mensajes
"insumo" y "resultado" se pasan entre padre e hijo por pipes. Nunca hay más de
**K** procesos vivos al mismo tiempo.

## Compilar

```sh
make
```

Genera `./planificador`. Equivale a:

```sh
gcc -Wall -Wextra -std=gnu17 -O2 -o planificador planificador.c
```

`make` usa `-std=gnu17` y no `-std=c17` a propósito: en modo ISO estricto
`c17` desaparecen las declaraciones POSIX (`fork`, `pipe`, `sigaction`,
`sigprocmask`, `nanosleep`) y el archivo no compila.

## Ejecutar

```sh
./planificador plan.txt K
```

- `plan.txt`: el plan de actividades.
- `K`: máximo de procesos simultáneos (entero >= 1).

Con el Makefile:

```sh
make run                        # usa plan_ejemplo.txt con K=3
make run PLAN=estres.txt K=10000
```

## Formato del plan

Una línea por actividad, con los campos separados por `:`:

```
id : nombre : tiempo_ms : dependencias
```

- **id**: identidad de la actividad, como texto. No puede repetirse.
- **nombre**: lo que se imprime; si empieza con `!` la actividad **falla**
  a propósito (el `!` no forma parte del nombre).
- **tiempo_ms**: duración simulada. Si va vacío, sale al azar entre 100 y
  5000 ms.
- **dependencias**: IDs separados por coma, de los que esta actividad
  necesita el resultado. Vacío = no depende de nadie.

Se ignoran los espacios sobrantes al principio y al final de cada campo.

```
1 : prender_carbon : 500 :
2 : comprar_carne : 1200 :
3 : comprar_pan : 300 :
4 : asar_longaniza : 800 : 1, 2
5 : armar_choripan : 250 : 3, 4
6 : servir_mesa : 100 : 5
```

## Qué imprime

Por cada actividad, cuando arranca y cuando termina:

```
  [1] prender_carbon: empieza (500 ms). Insumos: []
Termino [1] prender_carbon -> mensaje: "prender_carbon listo"
  [4] asar_longaniza: empieza (800 ms). Insumos: [1(prender_carbon): prender_carbon listo; 2(comprar_carne): comprar_carne listo; ]
Termino [4] asar_longaniza -> mensaje: "asar_longaniza listo"
```

Al final:

```
Maximo de procesos simultaneos: 3 (K = 3)
Resumen: 6 ok, 0 fallidas, 0 abortadas por falla
```

- **Maximo de procesos simultaneos** es el pico real de hijos vivos, y nunca
  supera K. Puede quedar bastante por debajo: lo que limita el paralelismo es
  la forma del grafo, no K. En `estres.txt` (10 000 actividades) el pico son
  unos 1500 aunque se le dé K = 10 000.
- **Resumen** cuenta las tres salidas posibles de una actividad: terminó bien,
  terminó con error, o nunca corrió porque falló algo de lo que dependía.

Cuando una actividad falla, se aborta en cascada todo lo que dependía de ella:

```
Fallo [2] b
Aborto [3] c (depende de una actividad fallida)
```

## Códigos de salida

| Código | Cuándo |
|---|---|
| 0 | Todo resolvió: `ok + fallidas + abortadas == total` |
| 1 | Plan inválido, error de sistema, o el plan tiene un ciclo |
| 130 | Interrumpido con Ctrl+C (128 + SIGINT) |

Un plan con ciclo se detecta al final, cuando ya no queda nada por lanzar ni
por esperar y sobran actividades sin resolver:

```
Error: el plan tiene un ciclo (0 de 2 actividades resueltas).
```

## Errores del plan

| Mensaje | Causa |
|---|---|
| `Error: '2' depende de 'fantasma', que no existe.` | dependencia con ID inexistente |
| `Error: ID repetido '1' (linea 3).` | dos actividades con el mismo ID |
| `Error: tiempo no numerico 'abc' en la actividad '2' (linea 2).` | el tiempo no es un entero |
| `Error: tiempo negativo '-5' ...` | duración negativa |

Los tres se detectan **antes** de crear cualquier proceso, así que no queda
nada corriendo cuando se imprimen.

## K y el límite de descriptores

Cada pipe es un descriptor de archivo, y el sistema impone un tope de
descriptores abiertos por proceso (`ulimit -n`). El padre guarda una pipe de
resultado por cada hijo vivo, así que un K enorme choca con ese tope. Por eso
al arrancar el programa:

1. **Sube el límite blando** con `setrlimit` hasta 65 536 o hasta el límite
   duro, lo que sea menor. Si tu `ulimit -Hn` es alto, esto basta y el programa
   corre sin decir nada.
2. **Si aun así no cabe, baja K** y avisa, dejando siempre margen para
   `stdin`/`stdout`/`stderr` y para la cola de lanzamiento:

   ```
   Aviso: K=10000 no cabe en el limite de descriptores (1024); uso K=1004
   ```

   El K efectivo aparece en la línea final (`(K = 1004)`).

Los dos casos son correctos: en ambos se cumple "nunca más de K procesos".

Para verlo:

```sh
ulimit -n 1024; ./planificador estres.txt 10000     # aviso, baja K
ulimit -Sn 1024; ./planificador estres.txt 10000    # lo sube, sin aviso
```

## Ctrl+C

Con Ctrl+C el programa no se muere de golpe: mata a los hijos que están
corriendo con `SIGTERM`, espera a cada uno con `waitpid` para no dejar
zombis, marca todo lo demás como abortado y sale con 130. Es el mismo cierre
que se usa si `pipe()` o `fork()` fallan, justamente para que un error del
sistema tampoco deje hijos huérfanos.

```
*** Llego la Seremi (Ctrl+C): abortando todas las actividades ***
Aborto [1] a (estaba en ejecucion)
Interrumpido: 2 en ejecucion cortadas, 2 sin lanzar
```

Los hijos ignoran `SIGINT`, así que el corte lo maneja solo el padre y no
importa si el Ctrl+C llega al grupo de procesos completo o solo al padre.

## Pruebas

```sh
make test
```

Cubre: plan válido, fallo con cascada, ciclo, tiempo al azar, los tres tipos de
plan inválido, y el estrés de 10 000 actividades con K = 10 000.

Los archivos de prueba:

| Archivo | Qué prueba |
|---|---|
| `plan_ejemplo.txt` | el plan de ejemplo de la consigna |
| `falla.txt` | una actividad con `!` y la cascada de abortadas |
| `ciclo.txt` | dos actividades que dependen la una de la otra |
| `largo.txt` | actividades de 5 s, para probar Ctrl+C |
| `vacio.txt` | tiempo al azar |
| `malo1.txt` | dependencia inexistente |
| `malo2.txt` | ID repetido |
| `malo3.txt` | tiempo no numérico |
| `estres.txt` | 10 000 actividades con dependencias al azar |

Ctrl+C a mano:

```sh
./planificador largo.txt 2     # en otra terminal, Ctrl+C
pgrep planificador             # no debe imprimir nada
```

## Decisiones de diseño

- **Sin threads**: cada actividad es un proceso, que es lo que pide la
  consigna, y aísla los fallos: si una actividad revienta, las demás siguen.
- **IDs como texto**, no como índices: el archivo es legible y el orden de las
  líneas no importa para nombrar. El ID se traduce a posición una sola vez
  (pasada de dependencias).
- **Lista inversa de sucesores**: cuando una actividad termina, solo se
  recorren las que dependen de ella, en vez de revisar las 10 000.
- **Búsqueda lineal** para traducir ID a posición: 10 000 comparaciones de
  texto por dependencia son nada al lado de crear 10 000 procesos.
- **`waitpid` con `sigsuspend`** en vez de polling: el padre bloquea
  `SIGINT` y `SIGCHLD` y duerme con `sigsuspend`, que solo vuelve cuando llega
  alguna de las dos. Así no se pierde ninguna señal y no hay busy loop.
- **Aleatorio de veras**: `srand(time(NULL))` sin `srand` fijo, para que dos
  corridas den tiempos distintos.
- **Tope de 10 000 actividades** (`MAX_ACTIVIDADES`).

## Archivos

| Archivo | Qué es |
|---|---|
| `planificador.c` | el programa completo, un solo archivo |
| `Makefile` | `all`, `run`, `test`, `clean` |
| `paso1.c` … `paso10.c` | la construcción paso a paso (ver abajo) |
| `*.txt` | los planes de prueba |

### Los pasos

Cada `pasoN.c` es el estado del programa después de esa pieza, y el siguiente
arranca copiando el anterior. Sirven para mostrar el orden en que se construyó
y para depurar por partes; para entregar basta `planificador.c`.

| Paso | Qué se agrega |
|---|---|
| `paso1.c` | lee el archivo y muestra las líneas |
| `paso2.c` | parte cada línea por `:` en 4 campos, con `recortar` |
| `paso3.c` | estructura `Actividad` y arreglo `lista` |
| `paso4.c` | traduce las dependencias de texto a posiciones |
| `paso5a.c` | `fork` respetando el límite K (sin dormir todavía) |
| `paso5b.c` | la simulación: `nanosleep` del tiempo de cada actividad |
| `paso6.c` | pipes: insumos de entrada y mensaje de resultado |
| `paso7.c` | fallos aislados por rama, con la cascada de abortadas |
| `paso8.c` | Ctrl+C limpio: sin zombis ni señales perdidas |
| `paso9.c` | límite de descriptores y cierre común ante error del sistema |
| `paso10.c` | validaciones: ID repetido y tiempo no numérico |

