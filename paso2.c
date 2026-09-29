#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

void recortar(char *s) {
    /* 1) buscar dónde empieza el texto real (saltando espacios) */
    char *ini = s;
    while (*ini == ' ' || *ini == '\t' || *ini == '\n' || *ini == '\r') {
        ini++;
    }
    /* 2) mover el texto al principio del arreglo */
    memmove(s, ini, strlen(ini) + 1);

    /* 3) borrar los espacios del final, retrocediendo desde el último */
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

    char linea[4096];
    while (fgets(linea, sizeof(linea), f) != NULL) {

        /* campos[0]=ID, campos[1]=nombre, campos[2]=tiempo, campos[3]=deps
           Los dejamos apuntando a "" (texto vacío) por si la línea trae menos. */
        char *campos[4] = {"", "", "", ""};

        /* Cortar la línea en los ':'.
           p va avanzando por el texto; cada campo empieza donde está p. */
        char *p = linea;
        for (int i = 0; i < 4; i++) {
            campos[i] = p;                    /* este campo empieza aquí */
            char *dos_puntos = strchr(p, ':'); /* buscar el siguiente ':' */
            if (dos_puntos == NULL) {
                break;                        /* no hay más ':' -> último campo */
            }
            *dos_puntos = '\0';               /* cambiar el ':' por fin de texto */
            p = dos_puntos + 1;               /* el próximo campo empieza después */
        }

        /* Limpiar los espacios de cada campo */
        for (int i = 0; i < 4; i++) {
            recortar(campos[i]);
        }

        /* Mostrar con [ ] para ver que no quedaron espacios pegados */
        printf("ID=[%s] nombre=[%s] tiempo=[%s] deps=[%s]\n",
               campos[0], campos[1], campos[2], campos[3]);
    }

    fclose(f);
    return 0;
}
