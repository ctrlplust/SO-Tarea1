#include <stdio.h>
#include <string.h>

/* s es un puntero al texto: se limpia en el mismo lugar y no devuelve nada,
   el que llama ya ve el texto sin espacios.
   ejemplo: " asar_longaniza \n"  ->  "asar_longaniza" */
void recortar(char *s) {

    /* 1) ini se va saltando los espacios del principio hasta topar con el
          primer caracter de verdad */
    char *ini = s;
    while (*ini == ' ' || *ini == '\t' || *ini == '\n' || *ini == '\r') {
        ini++;
    }

    /* 2) corremos el texto de verdad sobre los espacios de la izquierda.
          el +1 es pa que corra tambien el '\0' del final, y memmove en vez
          de memcpy porque origen y destino son el mismo arreglo */
    memmove(s, ini, strlen(ini) + 1);

    /* 3) desde el ultimo caracter de vuelta, cambiamos los espacios y el
          '\n' por '\0' hasta topar con texto de verdad */
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

    char linea[4096];
    while (fgets(linea, sizeof(linea), f) != NULL) {

        /* campos es un arreglo de 4 punteros: ID, nombre, tiempo y deps.
           arrancan apuntando a "" pa que, si la linea trae menos campos,
           no queden apuntando a basura */
        char *campos[4] = { vacio, vacio, vacio, vacio };

        /* p es un dedo que va pasando campo por campo. strchr busca el ':'
           que sigue y, si no hay mas, lo que queda es el ultimo campo */
        char *p = linea;
        for (int i = 0; i < 4; i++) {
            campos[i] = p;
            char *dos_puntos = strchr(p, ':');
            if (dos_puntos == NULL) {
                break;
            }
            /* en C un texto termina en '\0', asi que cambiar el ':' por '\0'
               ya corta el campo sin copiar un solo caracter */
            *dos_puntos = '\0';
            p = dos_puntos + 1;
        }

        /* los campos salieron con espacios pegados, los limpiamos */
        for (int i = 0; i < 4; i++) {
            recortar(campos[i]);
        }

        /* los corchetes muestran donde empieza y termina cada campo, asi se
           ve que no quedaron espacios sobrando */
        printf("ID=[%s] nombre=[%s] tiempo=[%s] deps=[%s]\n",
               campos[0], campos[1], campos[2], campos[3]);
    }

    fclose(f);
    return 0;
}
