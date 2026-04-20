#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static long get_file_size(FILE *fp) {
    long cur = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, cur, SEEK_SET);
    return size;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Uso: %s <arquivo_original> <arquivo_recuperado>\n", argv[0]);
        return 1;
    }

    FILE *fa = fopen(argv[1], "rb");
    FILE *fb = fopen(argv[2], "rb");

    if (!fa || !fb) {
        printf("Erro: nao foi possivel abrir os arquivos.\n");
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 1;
    }

    long size_a = get_file_size(fa);
    long size_b = get_file_size(fb);

    rewind(fa);
    rewind(fb);

    if (size_a != size_b) {
        printf("DIVERGENTE\n");
        printf("Tamanho original : %ld bytes\n", size_a);
        printf("Tamanho recuperado: %ld bytes\n", size_b);
        fclose(fa);
        fclose(fb);
        return 2;
    }

    long pos = 0;
    int ca, cb;
    while ((ca = fgetc(fa)) != EOF && (cb = fgetc(fb)) != EOF) {
        if (ca != cb) {
            printf("DIVERGENTE\n");
            printf("Primeira diferenca no byte %ld\n", pos);
            printf("Original : 0x%02X\n", (unsigned char)ca);
            printf("Recuperado: 0x%02X\n", (unsigned char)cb);
            fclose(fa);
            fclose(fb);
            return 2;
        }
        pos++;
    }

    fclose(fa);
    fclose(fb);

    printf("IGUAL\n");
    printf("Arquivos identicos (%ld bytes)\n", size_a);
    return 0;
}