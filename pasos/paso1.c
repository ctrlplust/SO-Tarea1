#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Uso: %s plan.txt\n", argv[0]);
        return 1;
    }

    /* fopen devuelve un puntero al archivo en modo lectura, o NULL si no lo
       pudo abrir. ahi perror nos dice cual fue el error */
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror("No se pudo abrir el archivo");
        return 1;
    }

    /* linea es donde va cayendo cada linea del archivo, y n va contando
       cuantas llevamos para poder numerarlas en la salida */
    char linea[4096];
    int n = 0;

    /* fgets copia la proxima linea en linea y devuelve el puntero.
       cuando ya no queda ninguna devuelve NULL, y ahi se corta el while */
    while (fgets(linea, sizeof(linea), f) != NULL) {
        n++;
        printf("linea %d: %s", n, linea);
    }

    fclose(f);
    return 0;
}
