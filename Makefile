CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude

ifeq ($(OS),Windows_NT)
	EXEEXT = .exe
	RM = del /Q
	RUN_PREFIX = .\\
	RMFILES = $(subst /,\,$(OBJ) $(TARGET) $(TOOLS) $(TEST_TARGET) bench_compare_tmp.txt helix_test_*.bin helix_test_*.dna helix_test_*_out.bin)
else
	EXEEXT =
	RM = rm -f
	RUN_PREFIX = ./
	RMFILES = $(OBJ) $(TARGET) $(TOOLS) $(TEST_TARGET) bench_compare_tmp.txt helix_test_*.bin helix_test_*.dna helix_test_*_out.bin
endif

TARGET = helix$(EXEEXT)
TOOLS = helix_compare$(EXEEXT) helix_mutate$(EXEEXT) helix_bench$(EXEEXT)
TEST_TARGET = helix_tests$(EXEEXT)

CLI_SRC = src/cli/helix_cli.c

CORE_SRC = \
	src/core/codec.c \
	src/core/crc32.c \
	src/core/file_format.c \
	src/dna/dna_codec.c \
	src/dna/oligo_constraints.c \
	src/ecc/gf256.c \
	src/ecc/rs.c

SRC = $(CLI_SRC) $(CORE_SRC)
OBJ = $(SRC:.c=.o)
CLI_OBJ = $(CLI_SRC:.c=.o)
CORE_OBJ = $(CORE_SRC:.c=.o)

all: $(TARGET) $(TOOLS)

$(TARGET): $(CLI_OBJ) $(CORE_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(CLI_OBJ) $(CORE_OBJ)

helix_compare$(EXEEXT): tools/helix_compare.c
	$(CC) $(CFLAGS) -o $@ tools/helix_compare.c

helix_mutate$(EXEEXT): tools/helix_mutate.c
	$(CC) $(CFLAGS) -o $@ tools/helix_mutate.c

helix_bench$(EXEEXT): tools/helix_bench.c $(CORE_OBJ)
	$(CC) $(CFLAGS) -o $@ tools/helix_bench.c $(CORE_OBJ)

$(TEST_TARGET): tests/helix_tests.c $(CORE_OBJ)
	$(CC) $(CFLAGS) -o $@ tests/helix_tests.c $(CORE_OBJ)

test: $(TEST_TARGET)
	$(RUN_PREFIX)$(TEST_TARGET)

clean:
	-$(RM) $(RMFILES)

rebuild: clean all
