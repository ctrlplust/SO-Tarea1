/* planificador.c -- Planificador de actividades con procesos y pipes.
 *
 * Cada linea del plan  "id : nombre : tiempo_ms : deps"  describe una
 * actividad.  Cada actividad se ejecuta en un proceso hijo (fork) y recibe
 * por una pipe los mensajes de las actividades de las que depende; al
 * terminar devuelve su propio mensaje por otra pipe.  Nunca hay mas de K
 * procesos vivos al mismo tiempo.
 *
 * Uso:  ./planificador plan.txt K
 */

/* La pauta compila con -std=c17, que es ISO estricto: en ese modo glibc esconde
   las declaraciones POSIX (fork, pipe, sigaction, nanosleep, sigprocmask, RLIMIT_NOFILE).
   Hay que pedirla explicitamente ANTES del primer #include, por eso esta linea
   va aca y no mas abajo. */
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
#define MSG_MAX 128
#define INSUMO_MAX 4096
#define DESCRIPTORES_MIN 65536     /* limite blando que intentsamos poner */
#define MARGEN 20                  /* margen para stdin/stdout/stderr y cola */

#define EST_PEND     0   /* todavia no termina (esperando o corriendo) */
#define EST_OK       1   /* termino bien */
#define EST_FALLIDA  2   /* termino con error */
#define EST_ABORTADA 3   /* nunca termino: fallo un ancestro o llego Ctrl+C */

typedef struct {
    char id[64];            /* identidad de la actividad (no puede repetirse) */
    char nombre[128];
    int  tiempo_ms;
    int  aleatorio;         /* 1 si el tiempo salio al azar */
    char deps_txt[512];     /* dependencias tal como vinieron, como texto */

    int *deps;              /* indices de las actividades de las que depende */
    int  ndeps;
    int  pendientes;        /* cuantas de sus dependencias faltan por terminar */

    int *sucs;              /* indices de las actividades que dependen de esta */
    int  nsucs;

    pid_t pid;              /* proceso hijo que la ejecuta */
    int  fd_res;            /* descriptor de la pipe por la que devuelve el msg */
    char msg[MSG_MAX];

    int  falla;             /* 1 si el nombre venia con '!' (falla a proposito) */
    int  estado;
} Actividad;

static Actividad lista[MAX_ACTIVIDADES];

/* La levanta el handler de Ctrl+C; el planificador la revisa en el ciclo. */
static volatile sig_atomic_t interrumpido = 0;

void manejar_sigint(int s) {
    (void)s;
    interrumpido = 1;
}

/* No hace nada: existe solo para que SIGCHLD despierte a sigsuspend. */
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

/* Devuelve la posicion de la actividad con ese ID, o -1 si no existe.
   Con 10000 actividades y busqueda lineal es de sobra: son 10000 comparaciones
   de texto, nada comparado con el costo de crear 10000 procesos. */
int buscar(int n, const char *id) {
    for (int i = 0; i < n; i++) {
        if (strcmp(lista[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* Marca como abortadas todas las actividades que dependen (directa o
   indirectamente) de la que fallo.  Se recorre con una pila explicita para
   no usar recursion, que con cadenas largas de dependencias se pasaria del
   limite de la pila. */
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

    /* ---- K contra el limite de descriptores ----
       Cada hijo vivo deja en el padre 1 descriptor abierto (la pipe de su
       resultado), y al lanzar uno nuevo se usan 4 mas (2 de entrada, 2 de
       salida), ademas de stdin/stdout/stderr.  Por eso:
         1) subimos el limite blando hasta el duro (o 65536), y
         2) si aun asi no cabe, bajamos K, que sigue cumpliendo
            "nunca mas de K procesos a la vez". */
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {

        rlim_t deseado = DESCRIPTORES_MIN;
        if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < deseado) {
            deseado = lim.rlim_max;
        }
        if (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur < deseado) {
            lim.rlim_cur = deseado;
            setrlimit(RLIMIT_NOFILE, &lim);   /* si falla, seguimos igual */
            getrlimit(RLIMIT_NOFILE, &lim);   /* leemos el valor real */
        }

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

    /* ---- Lectura del plan: una linea por actividad ---- */
    while (fgets(linea, sizeof(linea), f) != NULL) {
        if (n >= MAX_ACTIVIDADES) {
            break;
        }

        /* Partimos la linea por ':' en 4 campos.  Cada ':' se cambia por
           '\0', asi el campo queda cortado sin copiar nada.  Si la linea
           trae menos campos, los que falten quedan como texto vacio. */
        char *campos[4] = {"", "", "", ""};
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

        /* Copiamos a la actividad: los punteros de 'campos' valen solo
           dentro de esta vuelta del while, porque fgets los pisa. */
        Actividad *a = &lista[n];
        snprintf(a->id, sizeof(a->id), "%s", campos[0]);
        snprintf(a->nombre, sizeof(a->nombre), "%s", campos[1]);
        snprintf(a->deps_txt, sizeof(a->deps_txt), "%s", campos[3]);

        /* El ID es la identidad de la actividad: si dos comparten ID, las
           dependencias apuntarian a la primera y la segunda quedaria
           imposible de referencia. */
        if (buscar(n, a->id) >= 0) {
            printf("Error: ID repetido '%s' (linea %d).\n", a->id, n + 1);
            fclose(f);
            return 1;
        }

        /* Un nombre con '!' delante marca una actividad que falla. */
        if (a->nombre[0] == '!') {
            memmove(a->nombre, a->nombre + 1, strlen(a->nombre));
            recortar(a->nombre);
            a->falla = 1;
        }

        /* Tiempo vacio: al azar entre T_MIN y T_MAX.  Si viene escrito tiene
           que ser un numero entero: atoi no avisa (ante "abc" devuelve 0 en
           silencio), asi que lo comprobamos antes con strtol. */
        if (campos[2][0] == '\0') {
            a->tiempo_ms = T_MIN + rand() % (T_MAX - T_MIN + 1);
            a->aleatorio = 1;
        } else {
            char *fin_num;
            errno = 0;
            long valor = strtol(campos[2], &fin_num, 10);

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

    /* ---- Dependencias: de texto ("1, 2") a posiciones ---- */
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
                lista[i].deps = realloc(lista[i].deps,
                                        sizeof(int) * (lista[i].ndeps + 1));
                lista[i].deps[lista[i].ndeps] = j;
                lista[i].ndeps++;
            }
            dep = strtok(NULL, ",");
        }
        lista[i].pendientes = lista[i].ndeps;
    }

    /* ---- Sucesores: la lista inversa, para no tener que recorrer todas las
       actividades cada vez que una termina ---- */
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < lista[i].ndeps; k++) {
            int j = lista[i].deps[k];
            lista[j].nsucs++;
        }
    }
    for (int j = 0; j < n; j++) {
        if (lista[j].nsucs > 0) {
            lista[j].sucs = malloc(sizeof(int) * lista[j].nsucs);
        }
        lista[j].nsucs = 0;
    }
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < lista[i].ndeps; k++) {
            int j = lista[i].deps[k];
            lista[j].sucs[lista[j].nsucs] = i;
            lista[j].nsucs++;
        }
    }

    /* ---- Cola inicial: las que no dependen de nadie ---- */
    int *cola = malloc(sizeof(int) * n);
    int *pila = malloc(sizeof(int) * n);
    int ini = 0, fin = 0;

    for (int i = 0; i < n; i++) {
        if (lista[i].pendientes == 0) {
            cola[fin] = i;
            fin++;
        }
    }

    /* ---- Senales ----
       Bloqueamos SIGINT y SIGCHLD mientras trabajamos y usamos sigsuspend
       para dormir sin que se pierda ninguna: si las dos estan bloqueadas,
       sigsuspend solo vuelve cuando llega alguna de las dos. */
    sigset_t bloqueadas, viejas;
    sigemptyset(&bloqueadas);
    sigaddset(&bloqueadas, SIGINT);
    sigaddset(&bloqueadas, SIGCHLD);
    sigprocmask(SIG_BLOCK, &bloqueadas, &viejas);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = manejar_sigint;
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = manejar_sigchld;
    sigaction(SIGCHLD, &sa, NULL);

    /* ---- Planificador ---- */
    int activos = 0;
    int max_activos = 0;
    int oks = 0;
    int fallidas = 0;
    int abortadas = 0;
    int fallo_sistema = 0;   /* 1 si pipe() o fork() fallaron */

    while (oks + fallidas + abortadas < n && !interrumpido && !fallo_sistema) {

        /* (a) Lanzar mientras haya listas en la cola y cupo (activos < K).
             Si pipe() o fork() fallan no hacemos return: marcamos el error y
             salimos, para que el cierre de mas abajo mate a los hijos vivos
             en vez de dejarlos huerfanos. */
        while (ini < fin && activos < K) {
            int i = cola[ini];
            ini++;

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

            fflush(stdout);      /* que el hijo no duplique nuestro buffer */
            pid_t pid = fork();
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
                signal(SIGINT, SIG_IGN);   /* el Ctrl+C es del padre */
                signal(SIGCHLD, SIG_DFL);
                sigprocmask(SIG_SETMASK, &viejas, NULL);

                close(res[0]);
                close(ent[1]);

                /* Lee los insumos que le mando el padre. */
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

                if (lista[i].falla) {
                    printf("  [%s] %s: FALLO\n", lista[i].id, lista[i].nombre);
                    fflush(stdout);
                    close(res[1]);
                    _exit(1);
                }

                /* Devuelve su mensaje de resultado por la otra pipe. */
                char salida[MSG_MAX];
                int largo = snprintf(salida, sizeof(salida), "%s listo", lista[i].nombre);
                if (write(res[1], salida, largo) < 0) {
                    perror("write hijo");
                }
                close(res[1]);
                _exit(0);
            }

            /* ---- PADRE ---- */
            close(res[1]);
            close(ent[0]);

            /* Le manda los mensajes de las actividades de las que depende. */
            char insumo[INSUMO_MAX];
            int len = 0;
            insumo[0] = '\0';
            for (int k = 0; k < lista[i].ndeps; k++) {
                int d = lista[i].deps[k];
                int w = snprintf(insumo + len, sizeof(insumo) - len,
                                 "%s(%s): %s; ", lista[d].id, lista[d].nombre, lista[d].msg);
                if (w < 0 || len + w >= (int)sizeof(insumo)) {
                    len = sizeof(insumo) - 1;
                    break;
                }
                len += w;
            }
            if (write(ent[1], insumo, len) < 0) {
                perror("write padre");
            }
            close(ent[1]);

            lista[i].pid = pid;
            lista[i].fd_res = res[0];
            activos++;
            if (activos > max_activos) max_activos = activos;
        }

        if (fallo_sistema) {
            break;
        }

        /* (b) Nadie corriendo y nada por lanzar: lo que falta es un ciclo. */
        if (activos == 0) {
            break;
        }

        /* (c) Esperar a que termine un hijo o a que llegue Ctrl+C. */
        int st;
        pid_t fin_pid;
        while ((fin_pid = waitpid(-1, &st, WNOHANG)) == 0) {
            if (interrumpido) {
                break;
            }
            sigsuspend(&viejas);
        }
        if (interrumpido) {
            break;
        }
        if (fin_pid < 0) {
            perror("waitpid");
            break;
        }
        activos--;

        /* (d) Ver cual era y que hizo. */
        for (int i = 0; i < n; i++) {
            if (lista[i].pid != fin_pid) {
                continue;
            }

            int bien = WIFEXITED(st) && WEXITSTATUS(st) == 0;

            if (bien) {
                ssize_t r = read(lista[i].fd_res, lista[i].msg, sizeof(lista[i].msg) - 1);
                if (r < 0) r = 0;
                lista[i].msg[r] = '\0';
                close(lista[i].fd_res);

                lista[i].estado = EST_OK;
                oks++;
                printf("Termino [%s] %s -> mensaje: \"%s\"\n",
                       lista[i].id, lista[i].nombre, lista[i].msg);

                /* Sus sucesores pierden una dependencia pendiente; los que se
                   quedan sin ninguna entran en la cola. */
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

                /* Los que dependian de ella no se pueden ejecutar. */
                abortadas += abortar_descendientes(i, pila);
            }
            break;
        }
    }

    /* ---- Cierre por Ctrl+C o por error del sistema ----
       No se hace return a mitad de la ejecucion: los hijos que estan corriendo
       se matan con SIGTERM y se esperan con waitpid, para que no queden
       huerfanos ni zombis. */
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
            if (lista[i].pid > 0) {
                int estado_hijo;
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

    /* Si quedo algo sin resolver sin que haya hubo corte ni fallo del
       sistema, es que el grafo tiene un ciclo. */
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
