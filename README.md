# planificador de actividades con procesos y pipes

el planificador lee un `plan.txt`, arma el grafo de actividades y corre cada una
en un proceso hijo. los mensajes de insumos y de resultados van por pipes, y nunca
hay mas de **K** hijos vivos al mismo tiempo.

las cosas que hay que tener a mano:

```sh
make                                    # compila
./planificador planes/plan_ejemplo.txt 3   # corre
make test                               # las pruebas
```

## como esta armado el repo

```
README.md          este archivo
Makefile           compilar, correr y probar
planificador.c     el entregable, en la raiz
pasos/             paso1.c ... paso10.c, la construccion por partes
planes/            los 10 planes de prueba en .txt
```

`planificador.c` se queda en la raiz a proposito: la pauta pide compilar con
`gcc -Wall -Wextra -std=c17 -lpthread -o planificador planificador.c` y ese
comando tiene que andar tal cual desde la raiz del repo.

## como compila

```sh
gcc -Wall -Wextra -std=c17 -lpthread -o planificador planificador.c
```

son los flags que pide la pauta, y compila sin un solo warning. `planificador.c`
esta en la raiz a proposito, pa que ese comando se pueda correr tal cual desde
arriba. los once pasos de `pasos/` compilan con el mismo comando y sin warnings
tambien, y `make test-pasos` los revisa todos de una.

el detalle que hace falta: con `-std=c17` (que es ISO estricto) glibc esconde las
declaraciones POSIX, asi que el archivo arranca con

```c
#define _POSIX_C_SOURCE 200809L
```

**antes del primer `#include`**. sin esa linea no compila: tiran
`unknown type name 'sigset_t'` o `implicit declaration of function 'fork'`.
`-lpthread` va en el comando porque lo pide la pauta, aunque el programa no usa
hilos: usa procesos.

## como se ejecuta

```sh
./planificador plan.txt K
```

- `plan.txt`: el plan de actividades.
- `K`: maximo de procesos vivos a la vez (entero >= 1).

con el Makefile:

```sh
make run                              # planes/plan_ejemplo.txt con K=3
make run PLAN=planes/estres.txt K=10000
```

## formato del plan

una linea por actividad, con los campos separados por `:`:

```
id : nombre : tiempo_ms : dependencias
```

- **id**: identidad de la actividad, texto. no puede repetirse.
- **nombre**: lo que se imprime. si arranca con `!` la actividad **falla** a
  proposito, y el `!` no forma parte del nombre.
- **tiempo_ms**: duracion simulada. si va vacio, sale al azar entre 100 y 5000 ms.
- **dependencias**: ids separados por coma. vacio = no depende de nadie.

los espacios sobrantes al principio y al final de cada campo se ignoran, y tambien
las lineas que traen menos de 4 campos (lo que falte queda vacio).

```
1 : prender_carbon : 500 :
2 : comprar_carne : 1200 :
3 : comprar_pan : 300 :
4 : asar_longaniza : 800 : 1, 2
5 : armar_choripan : 250 : 3, 4
6 : servir_mesa : 100 : 5
```

## lo que hay que mirar cuando corre

esto es lo importante de la ejecucion, y como se ve:

```
  [4] asar_longaniza: empieza (800 ms). Insumos: [1(prender_carbon): prender_carbon listo; 2(comprar_carne): comprar_carne listo; ]
Termino [4] asar_longaniza -> mensaje: "asar_longaniza listo"

Maximo de procesos simultaneos: 3 (K = 3)
Resumen: 6 ok, 0 fallidas, 0 abortadas por falla
```

- la linea `Insumos:` es la prueba de que las pipes van bien: el hijo recibio de
  veras los mensajes de las actividades de las que depende, y por eso no puede
  arrancar hasta que terminan.
- **maximo de procesos simultaneos** es el pico real de hijos vivos, y nunca pasa
  K. puede quedar bastante por debajo, porque lo que limita el paralelismo es la
  forma del grafo y no K: en `planes/estres.txt` (10 000 actividades) el pico son
  1500 aunque le des K = 10 000.
- **resumen** cuenta las tres salidas posibles de una actividad: termino bien,
  termino con error, o nunca corrio porque fallo algo de lo que dependia.

cuando una actividad falla, se aborta solo la rama que dependia de ella, y las
que no tenian nada que ver siguen su curso normal:

```
Fallo [2] b
Aborto [3] c (depende de una actividad fallida)
```

### el limite K en la practica

`K = 1` sale todo secuencial, uno atras de otro. `K = 2` o `K = 3` ya se ven
varios hijos corriendo al mismo tiempo. `K = 10000` sobre `planes/estres.txt` no alcanza
para 10 000 en paralelo, y no es un bug: el grafo no deja.

### Ctrl+C

con Ctrl+C el programa no se muere de golpe. mata a los hijos que estan corriendo
con `SIGTERM`, espera a cada uno con `waitpid` pa que no queden zombis, marca todo
lo demas como abortado y sale con 130:

```
*** Llego la Seremi (Ctrl+C): abortando todas las actividades ***
Aborto [1] a (estaba en ejecucion)
Interrumpido: 2 en ejecucion cortadas, 2 sin lanzar
```

los hijos ignoran `SIGINT`, asi que el corte lo maneja solo el padre y no importa
si el Ctrl+C llega al grupo de procesos completo o solo al padre. es el mismo
bloque de cierre que se usa si `pipe()` o `fork()` fallan, justamente pa que un
error del sistema tampoco deje hijos huerfanos.

## K contra el limite de descriptores

cada pipe es un descriptor de archivo, y el sistema pone un tope de descriptores
abiertos por proceso (`ulimit -n`). el padre guarda una pipe de resultado por cada
hijo vivo, asi que un K enorme choca con ese tope. por eso al arrancar:

1. **sube el limite blando** con `setrlimit` hasta 65 536 o hasta el limite duro,
   lo que sea menor. si tu `ulimit -Hn` es alto, esto basta y el programa corre
   sin decir nada.
2. **si aun asi no cabe, baja K** y avisa, dejando margen para
   `stdin`/`stdout`/`stderr` y para la cola de lanzamiento:

   ```
   Aviso: K=10000 no cabe en el limite de descriptores (1024); uso K=1004
   ```

   el K efectivo aparece en la linea final (`(K = 1004)`).

los dos casos estan bien: en los dos se cumple "nunca mas de K procesos".

para verlo:

```sh
ulimit -n 1024;  ./planificador planes/estres.txt 10000   # aviso, baja K
ulimit -Sn 1024; ./planificador planes/estres.txt 10000   # lo sube, sin aviso
```

## codigos de salida

| Codigo | Cuando |
|---|---|
| 0 | todo resolvio: `ok + fallidas + abortadas == total` |
| 1 | plan invalido, error del sistema, o el plan tiene un ciclo |
| 130 | interrumpido con Ctrl+C (128 + SIGINT) |

un plan con ciclo se detecta al final, cuando ya no queda nada por lanzar ni por
esperar y sobran actividades sin resolver:

```
Error: el plan tiene un ciclo (0 de 2 actividades resueltas).
```

## errores del plan

| Mensaje | Causa |
|---|---|
| `Error: '2' depende de 'fantasma', que no existe.` | dependencia con id inexistente |
| `Error: ID repetido '1' (linea 3).` | dos actividades con el mismo id |
| `Error: tiempo no numerico 'abc' en la actividad '2' (linea 2).` | el tiempo no es un entero |
| `Error: tiempo negativo '-5' ...` | duracion negativa |

los cuatro se detectan **antes** de crear cualquier proceso, asi que cuando se
imprimen no hay nada corriendo.

## pruebas

```sh
make test
```

cubre: plan valido, fallo con cascada, ciclo, tiempo al azar, lineas con menos de
4 campos, los tres tipos de plan invalido, y el estres de 10 000 actividades con
K = 10 000.

estan todos en `planes/`:

| Archivo | Que prueba |
|---|---|
| `planes/plan_ejemplo.txt` | el plan de ejemplo de la consigna |
| `planes/falla.txt` | una actividad con `!` y la cascada de abortadas |
| `planes/ciclo.txt` | dos actividades que dependen la una de la otra |
| `planes/largo.txt` | actividades de 5 s, para probar el Ctrl+C |
| `planes/vacio.txt` | tiempo al azar |
| `planes/corto.txt` | lineas con menos de 4 campos |
| `planes/malo1.txt` | dependencia inexistente |
| `planes/malo2.txt` | id repetido |
| `planes/malo3.txt` | tiempo no numerico |
| `planes/estres.txt` | 10 000 actividades con dependencias al azar |

el Ctrl+C hay que probarlo a mano, porque `make` no lo puede interrumpir:

```sh
./planificador planes/largo.txt 2   # en otra terminal, Ctrl+C
pgrep planificador                 # no debe imprimir nada
```

## los pasos

cada `pasoN.c` es el estado del programa despues de esa pieza, y el siguiente
arranca copiando el anterior. sirven pa mostrar el orden en que se construyo y pa
depurar por partes; para entregar basta `planificador.c`.

| Paso | Que se agrega |
|---|---|
| `pasos/paso1.c` | lee el archivo y muestra las lineas |
| `pasos/paso2.c` | parte cada linea por `:` en 4 campos, con `recortar` |
| `pasos/paso3.c` | estructura `Actividad` y arreglo `lista` |
| `pasos/paso4.c` | traduce las dependencias de texto a posiciones |
| `pasos/paso5a.c` | sucesores y orden de ejecucion (Kahn) simulado, sin procesos todavia |
| `pasos/paso5b.c` | `fork` de verdad, respetando el limite K, con la espera de cada actividad |
| `pasos/paso6.c` | pipes: insumos de entrada y mensaje de resultado |
| `pasos/paso7.c` | fallos aislados por rama, con la cascada de abortadas |
| `pasos/paso8.c` | Ctrl+C limpio: sin zombis ni señales perdidas |
| `pasos/paso9.c` | limite de descriptores y cierre comun ante error del sistema |
| `pasos/paso10.c` | validaciones: id repetido y tiempo no numerico |

`paso5a.c` y `paso5b.c` estan partidos a proposito: el primero muestra el orden de
ejecucion (que se puede hacer con dos contadores y sin un solo proceso), y el
segundo es el mismo orden pero con `fork` de verdad. verlos separados deja claro
que el algoritmo de Kahn y el `fork` son dos cosas distintas.

## limitaciones conocidas

- las dependencias de una actividad se guardan en 512 caracteres: una linea
  con cientos de dependencias se corta.
- las lineas en blanco no se saltan: se leen como una actividad sin ID.

## decisiones de diseño

- **sin threads**: cada actividad es un proceso, que es lo que pide la consigna, y
  aísla los fallos: si una actividad revienta, las demas siguen.
- **los campos del plan son arreglos fijos, no `char*`**: los punteros que da
  `strtok` apuntan adentro de `linea`, y `linea` se pisa en cada vuelta del
  `fgets`. con un `char*` estariamos guardando la direccion de un texto que ya no
  existe. las dependencias si son `int *`, porque esas se agrandan con `realloc`.
- **ids como texto, no como indices**: el archivo queda legible y el orden de las
  lineas no importa pa nombrar. el id se traduce a posicion una sola vez, en la
  pasada de dependencias.
- **la pasada de dependencias va despues de leer todo**: una actividad puede
  depender de otra que aparece mas abajo en el archivo, asi que si la buscáramos
  al leerla todavia no estaria en la lista.
- **busqueda lineal** para traducir id a posicion: 10 000 comparaciones de texto
  por dependencia son nada al lado de crear 10 000 procesos.
- **lista inversa de sucesores**: cuando una actividad termina, solo se recorren
  las que dependen de ella, en vez de revisar las 10 000.
- **`strtok` sobre una copia**: `strtok` va metiendo `'\0'` en el texto que va
  partiendo, asi que si lo aplicáramos sobre `deps_txt` lo dejaria mutilado.
- **`atoi` no sirve pa validar**: ante `"abc"` devuelve 0 en silencio y la
  actividad terminaria al instante. por eso el tiempo se comprueba con `strtol`
  antes, que si dice hasta donde leyo.
- **`waitpid` con `sigsuspend`** en vez de polling: el padre bloquea `SIGINT` y
  `SIGCHLD` y duerme con `sigsuspend`, que solo vuelve cuando llega alguna de las
  dos. asi no se pierde ninguna señal y no hay busy loop. con `waitpid` bloqueado
  sin `WNOHANG` el padre se quedaria esperando solo por el primer hijo, y el
  Ctrl+C no se veria hasta que terminara.
- **dos pipes por actividad**: una pipe va en un solo sentido, asi que el
  resultado (hijo -> padre) y los insumos (padre -> hijo) van por canales
  separados. con una sola se mezclarian los buffers.
- **`fflush(stdout)` antes del `fork`**: stdout tiene un buffer en memoria, y si
  queda con cosas pendientes el hijo hereda una copia y las vuelve a imprimir.
- **`_exit` y no `return` en el hijo**: con `return` el seguiria con el resto de
  `main` y el planificador se clonaria entero.
- **el padre cierra `ent[1]` apenas escribe**: ese cierre es lo que le avisa al
  hijo que no viene nada mas, y su `read` devuelve 0. si el padre lo dejara
  abierto, el hijo se quedaria bloqueado para siempre.
- **el padre guarda el `fd` del resultado y no lo lee al tiro**: leer en el
  momento del `fork` lo dejaria esperando a que el hijo termine, y el plan
  correria de a uno.
- **cada proceso cierra los extremos de pipe que no usa**: con 10 000 actividades
  cada descriptor de mas se nota, y es justo el limite que ajusta el `setrlimit`.
- **`volatile sig_atomic_t` para la bandera del corte**: es lo unico que se puede
  tocar con seguridad desde un manejador de señal, y el manejador solo levanta la
  bandera: adentro no se pueden llamar funciones de la biblioteca.
- **el hijo ignora `SIGINT`**: el corte lo maneja el padre, que va matando de a uno
  y asi decide el orden.
- **cualquier fallo de `pipe()` o `fork()` pasa por el mismo cierre que el
  Ctrl+C**: si nos vamos en el medio con un `return`, los hijos que ya estan
  corriendo quedan sin padre.
- **la lista es `static` y global**: 10 000 actividades ocupan unos 6,8 MB y la
  pila son 8 MB, asi que como variable local reventaba. `static` ademas la deja en
  cero al empezar, que es justo lo que queremos en las casillas sin usar.
- **aleatorio de veras**: `srand(time(NULL))` sin semilla fija, pa que dos
  corridas den tiempos distintos.
- **tope de 10 000 actividades** (`MAX_ACTIVIDADES`).

## archivos

| Ruta | Que es |
|---|---|
| `planificador.c` | el programa completo, un solo archivo, en la raiz |
| `Makefile` | `all`, `run`, `test`, `test-pasos`, `clean` |
| `pasos/` | la construccion paso a paso, `paso1.c` … `paso10.c` |
| `planes/` | los 10 planes de prueba en `.txt` |
