#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* time, nanosleep, struct timespec */
#include <unistd.h>     /* fork, _exit, pipe, read, write, close */
#include <sys/wait.h>   /* waitpid */
#include <errno.h>      /* errno, EINTR */

#define MAX_ACTIVIDADES 10000
#define T_MIN 100
#define T_MAX 5000
#define MSG_MAX 128      /* NUEVO: tamaño del mensaje que entrega cada actividad */
#define INSUMO_MAX 4096  /* NUEVO: máximo que le pasamos a un hijo (cabe en una pipe sin bloquear) */

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

    pid_t pid;        /* proceso hijo que la ejecuta */

    /* NUEVO */
    int  fd_res;      /* extremo de LECTURA de la pipe por donde el hijo me responde */
    char msg[MSG_MAX];/* mensaje que entregó al terminar */
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

    /* ---- Cola inicial ---- */
    int *cola = malloc(sizeof(int) * n);
    int ini = 0, fin = 0;

    for (int i = 0; i < n; i++) {
        if (lista[i].pendientes == 0) {
            cola[fin] = i;
            fin++;
        }
    }

    /* ---- Scheduler ---- */
    int activos = 0;
    int ejecutadas = 0;
    int max_activos = 0;

    while (ejecutadas < n) {

        /* (a) Lanzar mientras haya listas en la cola y cupo (activos < K) */
        while (ini < fin && activos < K) {
            int i = cola[ini];
            ini++;

            /* NUEVO: dos pipes para esta actividad.
               res[0]/res[1]: hijo -> padre (su resultado)
               ent[0]/ent[1]: padre -> hijo (los mensajes de sus insumos)
               En cada pipe, [0] es para LEER y [1] para ESCRIBIR. */
            int res[2], ent[2];
            if (pipe(res) < 0 || pipe(ent) < 0) {
                perror("pipe");
                return 1;
            }

            fflush(stdout);
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return 1;
            }

            if (pid == 0) {
                /* ---- HIJO ---- */
                close(res[0]);   /* el hijo no lee su resultado */
                close(ent[1]);   /* el hijo no escribe sus insumos */

                /* NUEVO: leer los mensajes que dejó el padre */
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

                /* NUEVO: mandar el resultado al padre y terminar */
                char salida[MSG_MAX];
                int largo = snprintf(salida, sizeof(salida), "%s listo", lista[i].nombre);
                if (write(res[1], salida, largo) < 0) {
                    perror("write hijo");
                }
                close(res[1]);
                _exit(0);
            }

            /* ---- PADRE ---- */
            close(res[1]);   /* el padre no escribe el resultado */
            close(ent[0]);   /* el padre no lee los insumos */

            /* NUEVO: armar el texto de insumos con los mensajes de las
               dependencias y dejarlo en la pipe de entrada del hijo. */
            char insumo[INSUMO_MAX];
            int len = 0;
            insumo[0] = '\0';
            for (int k = 0; k < lista[i].ndeps; k++) {
                int d = lista[i].deps[k];
                int w = snprintf(insumo + len, sizeof(insumo) - len,
                                 "%s(%s): %s; ", lista[d].id, lista[d].nombre, lista[d].msg);
                if (w < 0 || len + w >= (int)sizeof(insumo)) {
                    len = sizeof(insumo) - 1;   /* se llenó: cortamos aquí */
                    break;
                }
                len += w;
            }
            if (write(ent[1], insumo, len) < 0) {
                perror("write padre");
            }
            close(ent[1]);   /* cerrar avisa "no viene más" */

            lista[i].pid = pid;
            lista[i].fd_res = res[0];   /* lo guardamos para leer cuando termine */
            activos++;
            if (activos > max_activos) max_activos = activos;
        }

        /* (b) Nadie corriendo y nada por lanzar: lo que falta es un ciclo */
        if (activos == 0) {
            break;
        }

        /* (c) Esperar A UN hijo, durmiendo (sin busy-waiting). */
        int estado;
        pid_t fin_pid = waitpid(-1, &estado, 0);
        if (fin_pid < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }
        activos--;
        ejecutadas++;

        /* (d) Buscar cuál era, leer su mensaje y avisar a sus sucesores */
        for (int i = 0; i < n; i++) {
            if (lista[i].pid == fin_pid) {

                /* NUEVO: leer lo que el hijo dejó en la pipe y cerrarla */
                ssize_t r = read(lista[i].fd_res, lista[i].msg, sizeof(lista[i].msg) - 1);
                if (r < 0) r = 0;
                lista[i].msg[r] = '\0';
                close(lista[i].fd_res);

                printf("Termino [%s] %s -> mensaje: \"%s\"\n",
                       lista[i].id, lista[i].nombre, lista[i].msg);

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

    free(cola);
    for (int i = 0; i < n; i++) {
        free(lista[i].deps);
        free(lista[i].sucs);
    }
    return ejecutadas < n ? 1 : 0;
}
