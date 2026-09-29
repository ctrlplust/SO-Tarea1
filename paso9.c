#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>          /* time, nanosleep, struct timespec */
#include <unistd.h>        /* fork, _exit, pipe, read, write, close */
#include <sys/wait.h>      /* waitpid, WIFEXITED, WEXITSTATUS */
#include <sys/resource.h>  /* NUEVO: getrlimit, setrlimit */
#include <errno.h>         /* errno */
#include <signal.h>        /* sigaction, sigsuspend, kill, SIGINT... */

#define MAX_ACTIVIDADES 10000
#define T_MIN 100
#define T_MAX 5000
#define MSG_MAX 128
#define INSUMO_MAX 4096

#define EST_PEND     0   /* todavía no termina (esperando o corriendo) */
#define EST_OK       1   /* terminó bien */
#define EST_FALLIDA  2   /* terminó con error */
#define EST_ABORTADA 3   /* nunca terminó: falló un ancestro o llegó Ctrl+C */

typedef struct {
    char id[64];
    char nombre[128];
    int  tiempo_ms;
    int  aleatorio;
    char deps_txt[512];

    int *deps;
    int  ndeps;
    int  pendientes;

    int *sucs;
    int  nsucs;

    pid_t pid;
    int  fd_res;
    char msg[MSG_MAX];

    int  falla;
    int  estado;
} Actividad;

static Actividad lista[MAX_ACTIVIDADES];

/* Bandera que levanta el handler de Ctrl+C. */
static volatile sig_atomic_t interrumpido = 0;

void manejar_sigint(int s) {
    (void)s;
    interrumpido = 1;
}

/* No hace nada: existe solo para que SIGCHLD despierte a sigsuspend. */
void manejar_sigchld(int s) {
    (void)s;
}

void recortar(char *s) {
    char *ini = s;
    while (*ini == ' ' || *ini == '\t' || *ini == '\n' || *ini == '\r') {
        ini++;
    }
    memmove(s, ini, strlen(ini) + 1);

    int len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\n' || s[len - 1] == '\r')) {
        len--;
        s[len] = '\0';
    }
}

int buscar(int n, const char *id) {
    for (int i = 0; i < n; i++) {
        if (strcmp(lista[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

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

    /* ---- NUEVO: ajustar K al límite de descriptores ----
       Cada hijo vivo deja 1 descriptor abierto en el padre (la pipe de su
       resultado). Mientras se lanza uno nuevo se usan 4 más, y además están
       stdin, stdout y stderr. Por eso dejamos un margen de 20. */
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {

        /* 1) Intentamos subir el límite blando (hasta 65536 o el duro). */
        rlim_t deseado = 65536;
        if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < deseado) {
            deseado = lim.rlim_max;
        }
        if (lim.rlim_cur != RLIM_INFINITY && lim.rlim_cur < deseado) {
            lim.rlim_cur = deseado;
            setrlimit(RLIMIT_NOFILE, &lim);   /* si falla, seguimos igual */
            getrlimit(RLIMIT_NOFILE, &lim);   /* leemos el valor real */
        }

        /* 2) Si K sigue sin caber, lo bajamos y avisamos. */
        if (lim.rlim_cur != RLIM_INFINITY) {
            long maximo = (long)lim.rlim_cur - 20;
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

    /* ---- PASADA 1: leer líneas ---- */
    while (fgets(linea, sizeof(linea), f) != NULL) {
        if (n >= MAX_ACTIVIDADES) {
            break;
        }
        char *campos[4] = {"", "", "", ""};
        char *p = linea;
        for (int i = 0; i < 4; i++) {
            campos[i] = p;
            char *dos_puntos = strchr(p, ':');
            if (dos_puntos == NULL) {
                break;
            }
            *dos_puntos = '\0';
            p = dos_puntos + 1;
        }
        for (int i = 0; i < 4; i++) {
            recortar(campos[i]);
        }

        Actividad *a = &lista[n];
        snprintf(a->id, sizeof(a->id), "%s", campos[0]);
        snprintf(a->nombre, sizeof(a->nombre), "%s", campos[1]);
        snprintf(a->deps_txt, sizeof(a->deps_txt), "%s", campos[3]);

        if (a->nombre[0] == '!') {
            memmove(a->nombre, a->nombre + 1, strlen(a->nombre));
            recortar(a->nombre);
            a->falla = 1;
        }

        if (campos[2][0] == '\0') {
            a->tiempo_ms = T_MIN + rand() % (T_MAX - T_MIN + 1);
            a->aleatorio = 1;
        } else {
            a->tiempo_ms = atoi(campos[2]);
            a->aleatorio = 0;
        }
        n++;
    }
    fclose(f);

    /* ---- PASADA 2: "1, 2" -> posiciones ---- */
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

    /* ---- PASADA 3: calcular los sucesores ---- */
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

    /* ---- Cola inicial ---- */
    int *cola = malloc(sizeof(int) * n);
    int *pila = malloc(sizeof(int) * n);
    int ini = 0, fin = 0;

    for (int i = 0; i < n; i++) {
        if (lista[i].pendientes == 0) {
            cola[fin] = i;
            fin++;
        }
    }

    /* ---- Preparar las señales (igual que el paso 8) ---- */
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

    /* ---- Scheduler ---- */
    int activos = 0;
    int max_activos = 0;
    int oks = 0;
    int fallidas = 0;
    int abortadas = 0;
    int fallo_sistema = 0;   /* NUEVO: 1 si pipe() o fork() fallaron */

    while (oks + fallidas + abortadas < n && !interrumpido && !fallo_sistema) {

        /* (a) Lanzar mientras haya listas en la cola y cupo (activos < K) */
        while (ini < fin && activos < K) {
            int i = cola[ini];
            ini++;

            /* NUEVO: si algo falla, no salimos con return: marcamos el
               error y salimos del ciclo para que el cierre mate a los
               hijos vivos y no queden huérfanos. */
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

            fflush(stdout);
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
                signal(SIGINT, SIG_IGN);
                signal(SIGCHLD, SIG_DFL);
                sigprocmask(SIG_SETMASK, &viejas, NULL);

                close(res[0]);
                close(ent[1]);

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

        /* NUEVO: si el lanzamiento falló, no seguimos esperando */
        if (fallo_sistema) {
            break;
        }

        /* (b) Nadie corriendo y nada por lanzar: lo que falta es un ciclo */
        if (activos == 0) {
            break;
        }

        /* (c) Esperar a un hijo o a Ctrl+C, durmiendo y sin perder señales */
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

        /* (d) Buscar cuál era y ver cómo terminó */
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

                abortadas += abortar_descendientes(i, pila);
            }
            break;
        }
    }

    /* ---- Cierre por Ctrl+C (la Seremi) o por error del sistema ----
       NUEVO: ahora también se usa cuando pipe() o fork() fallaron. */
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

    if (interrumpido) return 130;
    if (fallo_sistema) return 1;
    return resueltas < n ? 1 : 0;
}
