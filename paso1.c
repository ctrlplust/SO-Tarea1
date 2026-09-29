#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Uso: %s plan.txt\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror("No se pudo abrir el archivo");
        return 1;
    }

    char linea[4096];
    int n = 0;
    while (fgets(linea, sizeof(linea), f) != NULL) {
        n++;
        printf("linea %d: %s", n, linea);
    }

    fclose(f);
    return 0;
}
