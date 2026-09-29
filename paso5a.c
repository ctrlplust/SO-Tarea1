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

    int *deps;        /* de quiénes dependo (posiciones) */
    int  ndeps;
    int  pendientes;  /* cuántas dependencias faltan por terminar */

    /* NUEVO: lo contrario de deps */
    int *sucs;        /* quiénes dependen de mí (posiciones) */
    int  nsucs;
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

    /* ---- PASADA 1: leer líneas (igual que el paso 4) ---- */
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

    /* ---- PASADA 2: "1, 2" -> posiciones (igual que el paso 4) ---- */
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

    /* ---- NUEVO, PASADA 3: calcular los sucesores ----
       Si "i" depende de "j", entonces "i" es sucesor de "j".
       Primero CONTAMOS cuántos sucesores tiene cada uno... */
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < lista[i].ndeps; k++) {
            int j = lista[i].deps[k];
            lista[j].nsucs++;
        }
    }
    /* ...luego reservamos memoria justa para cada lista de sucesores... */
    for (int j = 0; j < n; j++) {
        if (lista[j].nsucs > 0) {
            lista[j].sucs = malloc(sizeof(int) * lista[j].nsucs);
        }
        lista[j].nsucs = 0;   /* lo reutilizamos como contador al llenar */
    }
    /* ...y por último los llenamos. */
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < lista[i].ndeps; k++) {
            int j = lista[i].deps[k];
            lista[j].sucs[lista[j].nsucs] = i;
            lista[j].nsucs++;
        }
    }

    /* ---- NUEVO: simulación del orden de ejecución (algoritmo de Kahn) ---- */
    int *cola = malloc(sizeof(int) * n);   /* posiciones listas para ejecutar */
    int ini = 0, fin = 0;                  /* la cola va de cola[ini] a cola[fin-1] */

    /* 1) Entran a la cola las que no esperan a nadie */
    for (int i = 0; i < n; i++) {
        if (lista[i].pendientes == 0) {
            cola[fin] = i;
            fin++;
        }
    }

    /* 2) Mientras haya alguien en la cola: sacarlo, "ejecutarlo" y avisar
          a sus sucesores. Los que quedan con pendientes == 0 entran a la cola. */
    int ejecutadas = 0;
    while (ini < fin) {
        int i = cola[ini];
        ini++;

        printf("Ejecuto [%s] %s (%d ms)\n",
               lista[i].id, lista[i].nombre, lista[i].tiempo_ms);
        ejecutadas++;

        for (int k = 0; k < lista[i].nsucs; k++) {
            int s = lista[i].sucs[k];
            lista[s].pendientes--;
            if (lista[s].pendientes == 0) {
                cola[fin] = s;
                fin++;
            }
        }
    }

    /* 3) Si no se ejecutaron todas, alguien quedó esperando para siempre:
          hay un ciclo. */
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
