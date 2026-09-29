#include <stdio.h>    /* printf, fopen, fgets, snprintf */
#include <stdlib.h>   /* atoi, rand, srand */
#include <string.h>   /* strchr, strlen, memmove */
#include <time.h>     /* time (para sortear números distintos cada vez) */

#define MAX_ACTIVIDADES 10000   /* el enunciado pide hasta 10000 */
#define T_MIN 100               /* rango del tiempo al azar */
#define T_MAX 5000

/* Una actividad del plan. Aquí SÍ copiamos el texto, para que no
   dependa de "linea" (que fgets sobrescribe en cada vuelta). */
typedef struct {
    char id[64];          /* ID alfanumérico, como texto */
    char nombre[128];
    int  tiempo_ms;       /* ahora sí es número */
    int  aleatorio;       /* 1 si el tiempo venía vacío y lo sorteamos */
    char deps_txt[512];   /* dependencias, todavía como texto: "1, 2" */
} Actividad;

/* Arreglo global. Lo dejamos fuera de main porque 10000 actividades
   son varios MB y en la pila (variables locales) podrían reventar. */
static Actividad lista[MAX_ACTIVIDADES];

/* Igual que en el paso 2: quita espacios y saltos de línea de los bordes. */
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

    srand(time(NULL));   /* inicia el generador de números al azar */

    char linea[4096];
    int n = 0;           /* cuántas actividades llevamos guardadas */

    while (fgets(linea, sizeof(linea), f) != NULL) {

        /* Si ya no caben más actividades, dejamos de leer. */
        if (n >= MAX_ACTIVIDADES) {
            break;
        }

        /* --- Igual que el paso 2: cortar la línea en 4 campos --- */
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

        /* --- NUEVO: copiar los campos a la actividad número n --- */
        Actividad *a = &lista[n];   /* a apunta a la casilla n del arreglo */

        /* snprintf copia texto sin pasarse del tamaño del arreglo. */
        snprintf(a->id, sizeof(a->id), "%s", campos[0]);
        snprintf(a->nombre, sizeof(a->nombre), "%s", campos[1]);
        snprintf(a->deps_txt, sizeof(a->deps_txt), "%s", campos[3]);

        /* Tiempo: si viene vacío, sorteamos entre 100 y 5000.
           Si no, convertimos el texto "800" al número 800 con atoi. */
        if (campos[2][0] == '\0') {
            a->tiempo_ms = T_MIN + rand() % (T_MAX - T_MIN + 1);
            a->aleatorio = 1;
        } else {
            a->tiempo_ms = atoi(campos[2]);
            a->aleatorio = 0;
        }

        n++;   /* pasamos a la siguiente casilla */
    }

    fclose(f);

    /* Ya terminó la lectura. Mostramos lo guardado para comprobar
       que los datos siguen ahí aunque "linea" ya no los tenga. */
    printf("Actividades leidas: %d\n\n", n);
    for (int i = 0; i < n; i++) {
        printf("[%d] id=%s nombre=%s tiempo=%d ms%s deps=[%s]\n",
               i, lista[i].id, lista[i].nombre, lista[i].tiempo_ms,
               lista[i].aleatorio ? " (aleatorio)" : "",
               lista[i].deps_txt);
    }

    return 0;
}
