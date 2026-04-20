#include <stdio.h>
#include <string.h>
#include "helix/codec.h"
#include "helix/status.h"

static void print_usage(const char *prog) {
    printf("Helix-Engine v1\n");
    printf("Uso:\n");
    printf("  %s enc <input> <output>\n", prog);
    printf("  %s dec <input> <output>\n", prog);
}

static void print_status(helix_status_t st) {
    switch (st) {
        case HELIX_OK:
            printf("Operacao concluida com sucesso.\n");
            break;
        case HELIX_ERR_INVALID_ARG:
            printf("Erro: argumentos invalidos.\n");
            break;
        case HELIX_ERR_FILE_OPEN:
            printf("Erro: nao foi possivel abrir um dos arquivos.\n");
            break;
        case HELIX_ERR_FILE_READ:
            printf("Erro: falha de leitura.\n");
            break;
        case HELIX_ERR_FILE_WRITE:
            printf("Erro: falha de escrita.\n");
            break;
        case HELIX_ERR_BAD_HEADER:
            printf("Erro: cabecalho HELIX invalido.\n");
            break;
        case HELIX_ERR_BAD_DNA:
            printf("Erro: arquivo DNA contem bases invalidas.\n");
            break;
        case HELIX_ERR_UNCORRECTABLE:
            printf("Erro: corrupcao acima da capacidade de correcao.\n");
            break;
        case HELIX_ERR_CHECKSUM:
            printf("Erro: checksum final nao confere.\n");
            break;
        case HELIX_ERR_BAD_OLIGO:
            printf("Erro: conjunto de oligos invalido ou incompleto.\n");
            break;
        case HELIX_ERR_CONSTRAINT:
            printf("Erro: nao foi possivel satisfazer as constraints biologicas do oligo.\n");
            break;
        default:
            printf("Erro interno.\n");
            break;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    helix_status_t st;

    if (strcmp(argv[1], "enc") == 0) {
        st = helix_encode_file(argv[2], argv[3]);
    } else if (strcmp(argv[1], "dec") == 0) {
        st = helix_decode_file(argv[2], argv[3]);
    } else {
        print_usage(argv[0]);
        return 1;
    }

    print_status(st);
    return (st == HELIX_OK) ? 0 : 1;
}
