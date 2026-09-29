#include <stdio.h>    /* printf, fopen, fgets, snprintf */
#include <stdlib.h>   /* atoi, rand, srand */
#include <string.h>   /* strchr, strlen, memmove */
#include <time.h>     /* time, pa sortear un tiempo distinto cada vez */

#define MAX_ACTIVIDADES 10000   /* el enunciado pide hasta 10000 */
#define T_MIN 100
#define T_MAX 5000

/* cada campo es un arreglo de tamaño fijo y no un char* porque los campos
   apuntan adentro de linea, y linea se pisa en cada vuelta del fgets:
   con un char* estaríamos guardando la dirección de un texto que ya no existe */
typedef struct {
    char id[64];
    char nombre[128];
    int  tiempo_ms;
    int  aleatorio;       /* 1 si el tiempo vino vacío y lo sorteamos */
    char deps_txt[512];   /* las dependencias todavía como texto: "1, 2" */
} Actividad;

/* va afuera de main porque 10000 actividades ocupan unos 6,8 MB y la pila son
   8 MB: como variable local reventaba. static además la deja en cero al
   empezar, que es justo lo que queremos en las casillas que no usemos */
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

/* "" a secas es un texto en memoria de solo lectura, y recortar() escribe
   sobre el, asi que acá usamos una copia que sí se puede modificar.
   sirve para las líneas que traen menos de 4 campos */
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

    srand(time(NULL));   /* pa que el tiempo al azar salga distinto cada vez */

    char linea[4096];
    int n = 0;           /* cuántas actividades llevamos guardadas */

    while (fgets(linea, sizeof(linea), f) != NULL) {

        if (n >= MAX_ACTIVIDADES) {
            break;       /* el plan trae más de 10000, nos quedamos con las primeras */
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

        /* a apunta a la casilla n, pa no estar escribiendo lista[n].id y
           lista[n].nombre en cada línea */
        Actividad *a = &lista[n];

        /* snprintf y no strcpy porque el tamaño va como argumento: si el texto
           es más largo que el arreglo lo recorta, en vez de pasarse de largo
           y romper el arreglo. sizeof(a->id) es el 64 del id */
        snprintf(a->id, sizeof(a->id), "%s", campos[0]);
        snprintf(a->nombre, sizeof(a->nombre), "%s", campos[1]);
        snprintf(a->deps_txt, sizeof(a->deps_txt), "%s", campos[3]);

        /* si el tiempo vino vacío lo sorteamos entre 100 y 5000 ms: el +1 es
           pa que el rango incluya al 5000, sin él sería hasta 4999.
           si vino escrito, atoi lo pasa de texto a número */
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

    /* linea ya no tiene nada que ver con los datos, así que mostrar la lista
       al final es la prueba de que los campos quedaron copiados.
       ojo: a sigue apuntando a la última actividad, por eso acá va
       lista[i].id y no a->id */
    printf("Actividades leidas: %d\n\n", n);
    for (int i = 0; i < n; i++) {
        printf("[%d] id=%s nombre=%s tiempo=%d ms%s deps=[%s]\n",
               i, lista[i].id, lista[i].nombre, lista[i].tiempo_ms,
               lista[i].aleatorio ? " (aleatorio)" : "",
               lista[i].deps_txt);
    }

    return 0;
}
