#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* time, nanosleep, struct timespec */
#include <unistd.h>     /* fork, _exit */
#include <sys/wait.h>   /* waitpid */
#include <errno.h>      /* errno, EINTR */

#define MAX_ACTIVIDADES 10000
#define T_MIN 100
#define T_MAX 5000

typedef struct {
    char id[64];
    char nombre[128];
    int  tiempo_ms;
    int  aleatorio;
    char deps_txt[512];

    int *deps;        /* de quiénes dependo (posiciones) */
    int  ndeps;
    int  pendientes;  /* cuántas dependencias faltan por terminar */

    int *sucs;        /* quiénes dependen de mí (posiciones) */
    int  nsucs;

    pid_t pid;        /* proceso hijo que la ejecuta (0 si aún no) */
} Actividad;

static Actividad lista[MAX_ACTIVIDADES];

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

    /* ---- Cola inicial: las que no esperan a nadie ---- */
    int *cola = malloc(sizeof(int) * n);
    int ini = 0, fin = 0;

    for (int i = 0; i < n; i++) {
        if (lista[i].pendientes == 0) {
            cola[fin] = i;
            fin++;
        }
    }

    /* ---- Scheduler real. El padre es el único que decide. ---- */
    int activos = 0;        /* hijos vivos ahora mismo */
    int ejecutadas = 0;     /* terminadas */
    int max_activos = 0;    /* máximo simultáneo (para demostrar K) */

    while (ejecutadas < n) {

        /* (a) Lanzar mientras haya listas en la cola y cupo (activos < K) */
        while (ini < fin && activos < K) {
            int i = cola[ini];
            ini++;

            fflush(stdout);   /* evita que el hijo repita texto pendiente */
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return 1;
            }
            if (pid == 0) {
                /* ---- HIJO: simula el trabajo y termina ---- */
                printf("  [%s] %s: empieza (%d ms)\n",
                       lista[i].id, lista[i].nombre, lista[i].tiempo_ms);
                fflush(stdout);

                struct timespec espera;
                espera.tv_sec  = lista[i].tiempo_ms / 1000;
                espera.tv_nsec = (lista[i].tiempo_ms % 1000) * 1000000L;
                nanosleep(&espera, NULL);

                _exit(0);
            }
            /* ---- PADRE ---- */
            lista[i].pid = pid;
            activos++;
            if (activos > max_activos) max_activos = activos;
        }

        /* (b) Si no hay nadie corriendo y no queda nada por lanzar,
               lo que falta es inalcanzable (ciclo): salir */
        if (activos == 0) {
            break;
        }

        /* (c) Esperar A UN hijo, DURMIENDO (sin busy-waiting). */
        int estado;
        pid_t fin_pid = waitpid(-1, &estado, 0);
        if (fin_pid < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }
        activos--;
        ejecutadas++;

        /* (d) Buscar cuál actividad era y avisar a sus sucesores */
        for (int i = 0; i < n; i++) {
            if (lista[i].pid == fin_pid) {
                printf("Termino [%s] %s\n", lista[i].id, lista[i].nombre);
                for (int k = 0; k < lista[i].nsucs; k++) {
                    int s = lista[i].sucs[k];
                    lista[s].pendientes--;
                    if (lista[s].pendientes == 0) {
                        cola[fin] = s;
                        fin++;
                    }
                }
                break;
            }
        }
    }

    printf("\nMaximo de procesos simultaneos: %d (K = %d)\n", max_activos, K);

    if (ejecutadas < n) {
        printf("Error: el plan tiene un ciclo (%d de %d actividades ejecutables).\n",
               ejecutadas, n);
    }

    /* Liberar memoria */
    free(cola);
    for (int i = 0; i < n; i++) {
        free(lista[i].deps);
        free(lista[i].sucs);
    }
    return ejecutadas < n ? 1 : 0;
}
