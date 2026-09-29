/* planificador.c -- planificador de actividades con procesos y pipes.
 *
 * Cada línea del plan  "id : nombre : tiempo_ms : deps"  describe una
 * actividad.  Cada actividad corre en un proceso hijo (fork) y recibe por una
 * pipe los mensajes de las actividades de las que depende; al terminar
 * devuelve su propio mensaje por otra pipe.  Nunca hay más de K procesos
 * vivos al mismo tiempo.
 *
 * uso:  ./planificador plan.txt K
 */

/* la pauta compila con -std=c17, que es ISO estricto, y en ese modo glibc
   esconde las declaraciones POSIX (fork, pipe, sigaction, nanosleep,
   sigprocmask, RLIMIT_NOFILE). hay que pedirla explícitamente ANTES del primer
   #include, por eso esta línea va acá y no más abajo */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>          /* time, nanosleep, struct timespec */
#include <unistd.h>        /* fork, _exit, pipe, read, write, close */
#include <sys/wait.h>      /* waitpid, WIFEXITED, WEXITSTATUS */
#include <sys/resource.h>  /* getrlimit, setrlimit */
#include <errno.h>         /* errno, ERANGE */
#include <signal.h>        /* sigaction, sigprocmask, sigsuspend, kill */

#define MAX_ACTIVIDADES 10000
#define T_MIN 100                 /* tiempo aleatorio: minimo y maximo (ms) */
#define T_MAX 5000
#define MSG_MAX 128      /* el mensaje que entrega cada actividad */
#define INSUMO_MAX 4096  /* lo que le pasamos a un hijo: cabe en una pipe sin
                           quedarse esperando a que el otro la vacíe */
#define DESCRIPTORES_MIN 65536     /* el límite blando que intentamos poner */
#define MARGEN 20                  /* pa stdin/stdout/stderr y las pipes del
                                      que se está lanzando */

/* los cuatro estados posibles de una actividad */
#define EST_PEND     0   /* todavía no termina: esperando o corriendo */
#define EST_OK       1   /* terminó bien */
#define EST_FALLIDA  2   /* terminó con error */
#define EST_ABORTADA 3   /* nunca terminó: falló un ancestro o llegó Ctrl+C */

/* cada campo es un arreglo de tamaño fijo y no un char* porque los campos
   del archivo apuntan adentro de linea, y linea se pisa en cada vuelta del
   fgets: con un char* estaríamos guardando la dirección de un texto que ya
   no existe */
typedef struct {
    char id[64];            /* identidad de la actividad, no puede repetirse */
    char nombre[128];
    int  tiempo_ms;
    int  aleatorio;         /* 1 si el tiempo vino vacío y lo sorteamos */
    char deps_txt[512];     /* las dependencias todavía como texto: "1, 2" */

    int *deps;              /* posiciones de las actividades de las que depende */
    int  ndeps;
    int  pendientes;        /* cuántas le faltan por terminar */

    int *sucs;              /* lo contrario de deps: quiénes dependen de esta */
    int  nsucs;

    pid_t pid;              /* el proceso hijo que la ejecuta; 0 mientras no
                               se haya lanzado */
    int  fd_res;            /* extremo de lectura de la pipe por donde el hijo
                               nos manda su resultado */
    char msg[MSG_MAX];

    int  falla;             /* 1 si el nombre venía con '!': fallo a propósito */
    int  estado;            /* EST_PEND / EST_OK / EST_FALLIDA / EST_ABORTADA */
} Actividad;

/* va afuera de main porque 10000 actividades ocupan unos 6,8 MB y la pila son
   8 MB: como variable local reventaba. static además la deja en cero al
   empezar, que es justo lo que queremos en las casillas que no usemos */
static Actividad lista[MAX_ACTIVIDADES];

/* la bandera del corte. volatile sig_atomic_t es lo único que se puede tocar
   con seguridad desde un manejador de señal */
static volatile sig_atomic_t interrumpido = 0;

/* el manejador de SIGINT solo levanta la bandera y nada más: adentro de un
   manejador no se pueden llamar funciones de la biblioteca (printf y las
   señales se interrumpen entre sí), lo único seguro es dejar una bandera */
void manejar_sigint(int s) {
    (void)s;
    interrumpido = 1;
}

/* este no hace nada: existe pa que cuando un hijo termina y llegue SIGCHLD,
   el proceso se despierte del sigsuspend aunque no haya que hacer nada con
   esa señal */
void manejar_sigchld(int s) {
    (void)s;
}

/* Quita espacios, tabs y saltos de linea del inicio y del final del texto. */
void recortar(char *s) {
    char *ini = s;
    while (*ini == ' ' || *ini == '\t' || *ini == '\n' || *ini == '\r') {
        ini++;
    }
    memmove(s, ini, strlen(ini) + 1);   /* el "+ 1" copia tambien el '\0' */

    int len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\n' || s[len - 1] == '\r')) {
        len--;
        s[len] = '\0';
    }
}

/* recorre la lista comparando el id con strcmp y devuelve la posición en la
   que está, o -1 si no aparece. necesitamos la posición y no el id porque
   después vamos a leer lista[j] directo */
int buscar(int n, const char *id) {
    for (int i = 0; i < n; i++) {
        if (strcmp(lista[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* marca como ABORTADAS todas las que dependen, directo o indirectamente, de
   "origen", que acaba de fallar. va guardando en la pila a medida que avanza,
   pa no tener que usar recursion. devuelve cuántas abortó */
int abortar_descendientes(int origen, int *pila) {
    int tope = 0;
    int total = 0;

    pila[tope] = origen;
    tope++;

    while (tope > 0) {
        tope--;
        int x = pila[tope];

        for (int k = 0; k < lista[x].nsucs; k++) {
            int s = lista[x].sucs[k];
            /* solo las que siguen pendientes, pa no marcar dos veces la
               misma ni pisar una que ya terminó bien */
            if (lista[s].estado == EST_PEND) {
                lista[s].estado = EST_ABORTADA;
                printf("Aborto [%s] %s (depende de una actividad fallida)\n",
                       lista[s].id, lista[s].nombre);
                total++;
                pila[tope] = s;
                tope++;
            }
        }
    }
    return total;
}

/* "" a secas es un texto en memoria de solo lectura, y recortar() escribe
   sobre el, asi que acá usamos una copia que sí se puede modificar.
   sirve para las líneas que traen menos de 4 campos */
static char vacio[] = "";

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Uso: %s plan.txt K\n", argv[0]);
        return 1;
    }
    int K = atoi(argv[2]);
    if (K < 1) {
        printf("K debe ser >= 1\n");
        return 1;
    }

    /* K contra el límite de descriptores: cada pipe es un descriptor, y el
       sistema pone un tope (ulimit -n). como el padre guarda una pipe por
       cada hijo vivo, un K enorme choca con ese tope.
       primero subimos el límite blando lo que se pueda; si aun así no cabe,
       bajamos K, que igual sigue cumpliendo "nunca más de K a la vez" */
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {

        /* el tope que se puede pedir es el límite duro, nunca más */
        rlim_t deseado = DESCRIPTORES_MIN;
        if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < deseado) {
            deseado = lim.rlim_max;
        }
        if (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur < deseado) {
            lim.rlim_cur = deseado;
            /* si el setrlimit falla seguimos igual, y el getrlimit de abajo
               nos dice en qué quedó realmente la cosa */
            setrlimit(RLIMIT_NOFILE, &lim);
            getrlimit(RLIMIT_NOFILE, &lim);
        }

        /* si con el límite nuevo tampoco cabe K, lo bajamos y avisamos */
        if (lim.rlim_cur != RLIM_INFINITY) {
            long maximo = (long)lim.rlim_cur - MARGEN;
            if (maximo < 1) maximo = 1;
            if (K > maximo) {
                printf("Aviso: K=%d no cabe en el limite de descriptores (%ld); uso K=%ld\n",
                       K, (long)lim.rlim_cur, maximo);
                K = (int)maximo;
            }
        }
    }

    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror("No pude abrir el archivo");
        return 1;
    }

    srand(time(NULL));

    char linea[4096];
    int n = 0;

    /* lectura del plan: una línea por actividad */
    while (fgets(linea, sizeof(linea), f) != NULL) {
        if (n >= MAX_ACTIVIDADES) {
            break;
        }

                /* partimos la línea por ':' en 4 campos. cada ':' se cambia por '\0',
           así el campo queda cortado sin copiar nada */
        char *campos[4] = { vacio, vacio, vacio, vacio };
        char *p = linea;
        for (int i = 0; i < 4; i++) {
            campos[i] = p;
            char *dos_puntos = strchr(p, ':');
            if (dos_puntos == NULL) {
                break;                     /* lo que queda es el ultimo campo */
            }
            *dos_puntos = '\0';
            p = dos_puntos + 1;
        }
        for (int i = 0; i < 4; i++) {
            recortar(campos[i]);
        }

                /* a apunta a la casilla n, pa no estar escribiendo lista[n].id y
           lista[n].nombre en cada línea.
           copiar sí: los punteros de 'campos' valen solo en esta vuelta del
           while, porque fgets los pisa */
        Actividad *a = &lista[n];
        snprintf(a->id, sizeof(a->id), "%s", campos[0]);
        snprintf(a->nombre, sizeof(a->nombre), "%s", campos[1]);
        snprintf(a->deps_txt, sizeof(a->deps_txt), "%s", campos[3]);

                /* el ID no puede repetirse: es la identidad de la actividad, y si dos
           comparten ID las dependencias apuntarían a la primera y la segunda
           quedaría imposible de referenciar */
        if (buscar(n, a->id) >= 0) {
            printf("Error: ID repetido '%s' (linea %d).\n", a->id, n + 1);
            fclose(f);
            return 1;
        }

                /* el '!' en el nombre marca que esta actividad falla */
        if (a->nombre[0] == '!') {
            memmove(a->nombre, a->nombre + 1, strlen(a->nombre));
            recortar(a->nombre);
            a->falla = 1;
        }

                /* si el tiempo vino vacío lo sorteamos entre 100 y 5000 ms: el +1 es
           pa que el rango incluya al 5000, sin él sería hasta 4999.
           si vino escrito tiene que ser un número, y atoi no avisa de nada:
           ante "abc" devuelve 0 en silencio y la actividad terminaría al
           instante, así que antes lo comprobamos con strtol, que sí dice
           hasta dónde leyó */
        if (campos[2][0] == '\0') {
            a->tiempo_ms = T_MIN + rand() % (T_MAX - T_MIN + 1);
            a->aleatorio = 1;
        } else {
            char *fin_num;
            errno = 0;
            long valor = strtol(campos[2], &fin_num, 10);

            /* fin_num == campos[2] es que no leyó ningún dígito, y
               *fin_num != '\0' es que después del número había letras */
            if (fin_num == campos[2] || *fin_num != '\0' || errno == ERANGE) {
                printf("Error: tiempo no numerico '%s' en la actividad '%s'"
                       " (linea %d).\n", campos[2], a->id, n + 1);
                fclose(f);
                return 1;
            }
            if (valor < 0) {
                printf("Error: tiempo negativo '%s' en la actividad '%s'"
                       " (linea %d).\n", campos[2], a->id, n + 1);
                fclose(f);
                return 1;
            }
            a->tiempo_ms = (int)valor;
            a->aleatorio = 0;
        }
        n++;
    }
    fclose(f);

    /* esta vuelta va después de terminar de leer el archivo y no adentro del
       while de fgets, porque una actividad puede depender de otra que
       aparece más abajo: si la buscáramos al leerla, esa de abajo todavía no
       estaría en la lista.

       strtok va metiendo '\0' en el texto que va partiendo, así que si lo
       aplicáramos sobre deps_txt lo dejaría mutilado. por eso partimos una
       copia y deps_txt queda intacto.
       la primera vez se le pasa el texto, y después NULL pa que siga
       partiendo el mismo en vez de arrancar uno nuevo */
    for (int i = 0; i < n; i++) {
        char copia[512];
        snprintf(copia, sizeof(copia), "%s", lista[i].deps_txt);

        char *dep = strtok(copia, ",");
        while (dep != NULL) {
            recortar(dep);
            if (dep[0] != '\0') {
                int j = buscar(n, dep);
                if (j < 0) {
                    printf("Error: '%s' depende de '%s', que no existe.\n",
                           lista[i].id, dep);
                    return 1;
                }
                /* no sabemos de antemano cuántas dependencias trae la
                   línea, así que el arreglo se agranda de a una. realloc
                   devuelve el mismo puntero con el lugar nuevo (puede
                   moverlo a otra dirección), y por eso hay que guardárselo
                   de vuelta */
                lista[i].deps = realloc(lista[i].deps,
                                        sizeof(int) * (lista[i].ndeps + 1));
                lista[i].deps[lista[i].ndeps] = j;
                lista[i].ndeps++;
            }
            dep = strtok(NULL, ",");
        }
        /* pendientes parte igual a ndeps y baja de a 1 cada vez que termina
           una de las que depende, pa saber cuándo puede arrancar */
        lista[i].pendientes = lista[i].ndeps;
    }

    /* lista de sucesores: si i depende de j, entonces i es sucesor de j.
       la hacemos al revés de deps pa no tener que recorrer las 10000
       actividades cada vez que una termina: así vamos directo a las que
       estaban esperando a esa */
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < lista[i].ndeps; k++) {
            int j = lista[i].deps[k];
            /* primero contamos cuántos sucesores tiene cada una, pa saber
               cuánta memoria hay que reservar */
            lista[j].nsucs++;
        }
    }
    for (int j = 0; j < n; j++) {
        if (lista[j].nsucs > 0) {
            lista[j].sucs = malloc(sizeof(int) * lista[j].nsucs);
        }
        /* nsucs vuelve a 0: ahora es el índice del próximo hueco a llenar */
        lista[j].nsucs = 0;
    }
    /* y por último los llenamos */
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < lista[i].ndeps; k++) {
            int j = lista[i].deps[k];
            lista[j].sucs[lista[j].nsucs] = i;
            lista[j].nsucs++;
        }
    }

    /* cola inicial: las que no dependen de nadie */
    int *cola = malloc(sizeof(int) * n);
    int *pila = malloc(sizeof(int) * n);
    int ini = 0, fin = 0;

    for (int i = 0; i < n; i++) {
        if (lista[i].pendientes == 0) {
            cola[fin] = i;
            fin++;
        }
    }

    /* preparamos las señales.
       El truco entero está en que SIGINT y SIGCHLD estén bloqueadas siempre:
       así si llega una en el instante en que estamos comprobando algo, queda
       anotada como pendiente y no se pierde, y después la levanta el
       sigsuspend, que es el único momento en que se desbloquean */
    sigset_t bloqueadas, viejas;
    sigemptyset(&bloqueadas);
    sigaddset(&bloqueadas, SIGINT);
    sigaddset(&bloqueadas, SIGCHLD);
    sigprocmask(SIG_BLOCK, &bloqueadas, &viejas);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    /* sa_flags en 0 y no SA_RESTART, pa que una llamada interrumpida no se
       reanude sola por debajo */
    sa.sa_flags = 0;
    sa.sa_handler = manejar_sigint;
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = manejar_sigchld;
    sigaction(SIGCHLD, &sa, NULL);

    /* el padre es el único que decide: él lleva la cuenta de cuántos hijos
       tiene vivos y los va lanzando hasta el tope de K */
    int activos = 0;        /* hijos vivos ahora mismo */
    int max_activos = 0;    /* el pico, pa poder mostrar que nunca pasamos de K */
    int oks = 0;
    int fallidas = 0;
    int abortadas = 0;
    int fallo_sistema = 0;   /* 1 si pipe() o fork() fallaron */

    while (oks + fallidas + abortadas < n && !interrumpido && !fallo_sistema) {

        /* (a) lanzar mientras haya en la cola y cupo (activos < K).
             si pipe() o fork() fallan no hacemos return: nos iríamos en el
             medio y los hijos que ya están corriendo quedarían sin padre, así
             que marcamos el error y salimos del ciclo pa que el cierre de más
             abajo los mate */
        while (ini < fin && activos < K) {
            int i = cola[ini];
            ini++;

            /* dos pipes por actividad, porque una pipe va en un solo
               sentido: res es del hijo al padre (su resultado) y ent del
               padre al hijo (los mensajes de sus insumos).
               en cada una, [0] es el extremo de lectura y [1] el de escritura */
            int res[2], ent[2];
            if (pipe(res) < 0) {
                perror("pipe");
                fallo_sistema = 1;
                break;
            }
            if (pipe(ent) < 0) {
                perror("pipe");
                close(res[0]);
                close(res[1]);
                fallo_sistema = 1;
                break;
            }

            /* stdout tiene un buffer en memoria: si lo dejamos con cosas
               pendientes, el hijo hereda una copia y vuelve a imprimirlas.
               con fflush queda vacío antes de clonar */
            fflush(stdout);
            pid_t pid = fork();     /* devuelve 0 en el hijo y el pid del hijo
                                       en el padre: el if de abajo los separa */
            if (pid < 0) {
                perror("fork");
                close(res[0]);
                close(res[1]);
                close(ent[0]);
                close(ent[1]);
                fallo_sistema = 1;
                break;
            }

            if (pid == 0) {
                /* ---- HIJO ---- */
                /* el Ctrl+C le llega a todo el grupo de procesos, así que el
                   hijo lo ignora: el corte lo maneja el padre, que va
                   matando uno por uno y así decide el orden */
                signal(SIGINT, SIG_IGN);
                signal(SIGCHLD, SIG_DFL);
                sigprocmask(SIG_SETMASK, &viejas, NULL);

                /* cerramos los extremos que no usamos, pa no andar dejando
                   descriptores abiertos: con 10000 actividades cada uno que
                   sobra se nota */
                close(res[0]);
                close(ent[1]);

                /* leemos los insumos que dejó el padre. el -1 en el sizeof
                   deja un byte libre pa el '\0', porque la pipe manda los
                   bytes crudos y no pone el fin de texto */
                char recibido[INSUMO_MAX];
                ssize_t r = read(ent[0], recibido, sizeof(recibido) - 1);
                if (r < 0) r = 0;
                recibido[r] = '\0';
                close(ent[0]);

                printf("  [%s] %s: empieza (%d ms). Insumos: [%s]\n",
                       lista[i].id, lista[i].nombre, lista[i].tiempo_ms, recibido);
                fflush(stdout);

                struct timespec espera;
                espera.tv_sec  = lista[i].tiempo_ms / 1000;
                espera.tv_nsec = (lista[i].tiempo_ms % 1000) * 1000000L;
                nanosleep(&espera, NULL);

                /* el '!' en el nombre marca que esta actividad falla */
                if (lista[i].falla) {
                    printf("  [%s] %s: FALLO\n", lista[i].id, lista[i].nombre);
                    fflush(stdout);
                    close(res[1]);
                    _exit(1);
                }

                /* el resultado se va por la otra pipe */
                char salida[MSG_MAX];
                int largo = snprintf(salida, sizeof(salida), "%s listo", lista[i].nombre);
                if (write(res[1], salida, largo) < 0) {
                    perror("write hijo");
                }
                close(res[1]);
                /* _exit y no return: con return el seguiría con el resto de
                   main y el planificador se clonaría entero. además _exit no
                   vacía el buffer de stdout, así que el hijo no repite nada */
                _exit(0);
            }

            /* ---- PADRE ---- */
            close(res[1]);
            close(ent[0]);

            /* armamos el texto con los mensajes de las dependencias de esta
               actividad, que son los que ya guardamos en msg */
            char insumo[INSUMO_MAX];
            int len = 0;
            insumo[0] = '\0';
            for (int k = 0; k < lista[i].ndeps; k++) {
                int d = lista[i].deps[k];
                int w = snprintf(insumo + len, sizeof(insumo) - len,
                                 "%s(%s): %s; ", lista[d].id, lista[d].nombre, lista[d].msg);
                if (w < 0 || len + w >= (int)sizeof(insumo)) {
                    /* se llenó el arreglo: cortamos acá, porque si seguíamos
                       escribiendo pasaríamos el final */
                    len = sizeof(insumo) - 1;
                    break;
                }
                len += w;
            }
            if (write(ent[1], insumo, len) < 0) {
                perror("write padre");
            }
            /* cerrar el extremo de escritura es lo que le avisa al hijo que
               no viene nada más: su read devuelve 0 y sigue con su trabajo */
            close(ent[1]);

            lista[i].pid = pid;
            /* guardamos el extremo de lectura pa leer el mensaje más adelante.
               leerlo acá nos dejaría esperando a que el hijo termine, y el
               plan correría de a uno */
            lista[i].fd_res = res[0];
            /* el contador sube solo en el padre: la memoria del hijo es una
               copia y se pierde con el _exit, y el que lleva la cuenta de los
               cupos es el padre */
            activos++;
            if (activos > max_activos) max_activos = activos;
        }

        if (fallo_sistema) {
            break;                  /* el cierre de abajo se encarga */
        }

        /* (b) si no hay nadie corriendo y no queda nada por lanzar, nadie va a
               liberar un cupo: lo que falta es inalcanzable, hay un ciclo */
        if (activos == 0) {
            break;
        }

        /* (c) esperamos a un hijo o a Ctrl+C durmiendo de verdad: WNOHANG
               solo pregunta "¿terminó alguno?" sin parar el proceso, y si la
               respuesta es que no, sigsuspend nos duerme (0% de CPU)
               dándole las señales al mismo instante. si waited estuviera
               bloqueado sin WNOHANG, el proceso se quedaría esperando solo
               por el primer hijo y el Ctrl+C no se vería hasta que terminara */
        int st;
        pid_t fin_pid;
        while ((fin_pid = waitpid(-1, &st, WNOHANG)) == 0) {
            if (interrumpido) {
                break;
            }
            sigsuspend(&viejas);   /* acá vuelve el proceso, y con él la
                                       señal que estaba pendiente */
        }
        if (interrumpido) {
            break;
        }
        if (fin_pid < 0) {
            perror("waitpid");
            break;
        }
        activos--;

        /* (d) buscamos cuál de las nuestras era comparando los pids */
        for (int i = 0; i < n; i++) {
            if (lista[i].pid != fin_pid) {
                continue;
            }

            /* waitpid nos deja el motivo de terminación en st: WIFEXITED
               pregunta si el hijo salió por su cuenta y WEXITSTATUS saca el
               código con el que terminó (el hijo usa _exit(1) al fallar) */
            int bien = WIFEXITED(st) && WEXITSTATUS(st) == 0;

            if (bien) {
                /* leemos lo que dejó el hijo. el '\0' va a mano, porque la
                   pipe no lo trae y sin él el %s del printf se pasa de largo */
                ssize_t r = read(lista[i].fd_res, lista[i].msg, sizeof(lista[i].msg) - 1);
                if (r < 0) r = 0;
                lista[i].msg[r] = '\0';
                close(lista[i].fd_res);

                lista[i].estado = EST_OK;
                oks++;
                printf("Termino [%s] %s -> mensaje: \"%s\"\n",
                       lista[i].id, lista[i].nombre, lista[i].msg);

                /* los sucesores pierden una dependencia pendiente, y los que
                   se quedan sin ninguna entran en la cola */
                for (int k = 0; k < lista[i].nsucs; k++) {
                    int s = lista[i].sucs[k];
                    if (lista[s].estado != EST_PEND) {
                        continue;
                    }
                    lista[s].pendientes--;
                    if (lista[s].pendientes == 0) {
                        cola[fin] = s;
                        fin++;
                    }
                }
            } else {
                close(lista[i].fd_res);
                lista[i].estado = EST_FALLIDA;
                fallidas++;
                printf("Fallo [%s] %s\n", lista[i].id, lista[i].nombre);

                /* el fallo no se lleva el plan entero: solo se aborta lo que
                   dependía de esta actividad. las que no dependen de ella
                   siguen su curso normal */
                abortadas += abortar_descendientes(i, pila);
            }
            break;
        }
    }

    /* cierre por Ctrl+C (la Seremi) o por error del sistema: es el mismo
       bloque para los dos casos, así que un fallo de pipe o de fork también
       mata y cosecha a los hijos en vez de dejarlos huérfanos.
       acá no se hace return a mitad de la ejecución */
    if (interrumpido || fallo_sistema) {
        int cortadas = 0;
        int sin_lanzar = 0;

        if (interrumpido) {
            printf("\n*** Llego la Seremi (Ctrl+C): abortando todas las actividades ***\n");
        } else {
            printf("\n*** Error del sistema: abortando todas las actividades ***\n");
        }

        for (int i = 0; i < n; i++) {
            if (lista[i].estado != EST_PEND) {
                continue;
            }
            if (lista[i].pid > 0) {   /* fue lanzada y sigue viva */
                int estado_hijo;
                /* SIGTERM primero y recién después el waitpid: si esperáramos
                   primero, el SIGTERM no se comería nadie y el hijo seguiría
                   corriendo. así no quedan ni vivos ni zombis */
                kill(lista[i].pid, SIGTERM);
                waitpid(lista[i].pid, &estado_hijo, 0);
                close(lista[i].fd_res);
                printf("Aborto [%s] %s (estaba en ejecucion)\n",
                       lista[i].id, lista[i].nombre);
                cortadas++;
            } else {
                sin_lanzar++;
            }
            lista[i].estado = EST_ABORTADA;
        }
        printf("Interrumpido: %d en ejecucion cortadas, %d sin lanzar\n",
               cortadas, sin_lanzar);
    }

    printf("\nMaximo de procesos simultaneos: %d (K = %d)\n", max_activos, K);
    printf("Resumen: %d ok, %d fallidas, %d abortadas por falla\n", oks, fallidas, abortadas);

    /* si quedó algo sin resolver y no hubo corte ni fallo del sistema, es que
       el grafo tiene un ciclo */
    int resueltas = oks + fallidas + abortadas;
    if (!interrumpido && !fallo_sistema && resueltas < n) {
        printf("Error: el plan tiene un ciclo (%d de %d actividades resueltas).\n",
               resueltas, n);
    }

    free(cola);
    free(pila);
    for (int i = 0; i < n; i++) {
        free(lista[i].deps);
        free(lista[i].sucs);
    }

    if (interrumpido) return 130;      /* 128 + SIGINT, como un programa normal */
    if (fallo_sistema) return 1;
    return resueltas < n ? 1 : 0;
}
