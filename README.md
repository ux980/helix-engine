# HELIX

HELIX e um prototipo experimental em C para armazenamento digital em DNA.

Em poucas palavras: o projeto pega um arquivo binario, fragmenta esse arquivo em oligos logicos, aplica protecao local e redundancia estrutural, converte tudo em sequencias de DNA em texto e depois tenta reconstruir o arquivo original com verificacao final de integridade.

## Resumo rapido

- Converte binario em DNA e DNA em binario.
- Trabalha com oligos independentes e indexados.
- Usa Reed-Solomon por oligo.
- Usa double parity por stripe.
- Recupera reordenacao e multiplas perdas estruturais.
- Aplica constraints reversiveis para GC e homopolimeros.
- Usa header estruturado com metadados do codec.

## Inspiracao e base teorica

Este projeto foi inspirado e contextualizado principalmente a partir dos trabalhos fundadores de armazenamento digital em DNA, em especial:

- George Church, Yuan Gao e Sriram Kosuri
- Nick Goldman e colaboradores
- Yaniv Erlich e Dina Zielinski

Artigos-base para este projeto:

- George Church, Yuan Gao e Sriram Kosuri, `Next-Generation Digital Information Storage in DNA` (`Science`, 2012)
- Nick Goldman et al., `Toward practical, high-capacity, low-maintenance information storage in synthesized DNA` (`Nature`, 2013)
- Yaniv Erlich e Dina Zielinski, `DNA Fountain enables a robust and efficient storage architecture` (`Science`, 2017)


## Compilacao

```bash
make
make test
```

## Uso basico

```bash
./helix enc arquivo.bin arquivo.dna
./helix dec arquivo.dna arquivo_recuperado.bin
```

No Windows, os binarios gerados pelo projeto usam `.exe`:

```bash
helix.exe enc arquivo.bin arquivo.dna
helix.exe dec arquivo.dna arquivo_recuperado.bin
```

## Ferramentas auxiliares


### `helix_mutate`

Aplica substituicoes aleatorias nas bases do arquivo `.dna`, preservando o header.

```bash
./helix_mutate entrada.dna saida_mutada.dna 0.01 12345
```

- terceiro argumento: taxa de substituicao entre `0.0` e `1.0`
- quarto argumento: `seed` opcional
- use `-noseed` para fluxo nao deterministico

Exemplo no Windows:

```bash
helix_mutate.exe entrada.dna saida_mutada.dna 0.01 12345
```

### `helix_compare`

Compara o arquivo original com o arquivo recuperado e informa se o resultado esta identico ou onde aparece a primeira divergencia.

```bash
./helix_compare arquivo_original.bin arquivo_recuperado.bin
```

Exemplo no Windows:

```bash
helix_compare.exe arquivo_original.bin arquivo_recuperado.bin
```

### `helix_bench`

Executa um fluxo automatizado de benchmark:

1. codifica o arquivo;
2. aplica mutacao ou alteracao estrutural;
3. tenta decodificar;
4. registra o resultado em `.csv`.

Uso:

```bash
./helix_bench entrada.bin encoded.dna mutated.dna decoded.bin mutation_spec resultados.csv 12345
```

Exemplos com substituicao por base:

```bash
./helix_bench entrada.bin encoded.dna mutated.dna decoded.bin 0.01 bench.csv 12345
```

Exemplos com mutacoes estruturais:

```bash
./helix_bench entrada.bin encoded.dna mutated.dna decoded.bin shuffle bench.csv 12345
./helix_bench entrada.bin encoded.dna mutated.dna decoded.bin drop1 bench.csv 12345
./helix_bench entrada.bin encoded.dna mutated.dna decoded.bin drop_parity bench.csv 12345
./helix_bench entrada.bin encoded.dna mutated.dna decoded.bin duplicate_replace bench.csv 12345
./helix_bench entrada.bin encoded.dna mutated.dna decoded.bin drop2same bench.csv 12345
```

Exemplo no Windows:

```bash
helix_bench.exe entrada.bin encoded.dna mutated.dna decoded.bin 0.01 bench.csv 12345
```