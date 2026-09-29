#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ACTIVIDADES 10000
#define T_MIN 100
#define T_MAX 5000

typedef struct {
    char id[64];
    char nombre[128];
    int  tiempo_ms;
    int  aleatorio;
    char deps_txt[512];

    int *deps;
    int  ndeps;
    int  pendientes;
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

static char vacio[] = "";

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Uso: %s plan.txt\n", argv[0]);
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

    while (fgets(linea, sizeof(linea), f) != NULL) {
        if (n >= MAX_ACTIVIDADES) {
            break;
        }

        char *campos[4] = { vacio, vacio, vacio, vacio };
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

    printf("Actividades leidas: %d\n\n", n);
    for (int i = 0; i < n; i++) {
        printf("[%d] id=%s nombre=%s tiempo=%d ms%s pend=%d\n",
               i, lista[i].id, lista[i].nombre, lista[i].tiempo_ms,
               lista[i].aleatorio ? " (aleatorio)" : "",
               lista[i].pendientes);
        printf("     depende de posiciones:");
        for (int k = 0; k < lista[i].ndeps; k++) {
            printf(" %d(id %s)", lista[i].deps[k], lista[lista[i].deps[k]].id);
        }
        printf("\n");
    }

    for (int i = 0; i < n; i++) {
        free(lista[i].deps);
    }
    return 0;
}
