# SkyBook - make / make bench / make test / make clean

CC     = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic

ifeq ($(OS),Windows_NT)
  EXE     := .exe
  CFLAGS  += -D__USE_MINGW_ANSI_STDIO=1
else
  EXE     :=
endif

CORE_OBJ = record.o array_index.o bst.o hash_open.o hash_chain.o secindex.o pds.o skybook.o
APP      = skybook$(EXE)
TESTS    = skybook_tests$(EXE)

all: $(APP)

$(APP): $(CORE_OBJ) bench.o main.o
	$(CC) $(CFLAGS) -o $@ $^

$(TESTS): $(CORE_OBJ) tests.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c $(wildcard *.h)
	$(CC) $(CFLAGS) -c $<

bench: $(APP)
	./$(APP) --bench

test: $(TESTS)
	./$(TESTS)

TEMP_DIRS  = skybench skytest skytest2 skytest3 skytest4
TEMP_FILES = skybench_io.bin skybench_io.csv

# From PowerShell/cmd (no sh.exe on PATH) make reports SHELL as a bare
# "sh.exe" and runs recipes through cmd.exe, which has no rm.
ifeq ($(SHELL),sh.exe)
clean:
	-del /Q *.o $(APP) $(TESTS) $(TEMP_FILES) 2>NUL
	for %%d in ($(TEMP_DIRS)) do @if exist %%d rmdir /S /Q %%d
else
clean:
	rm -f *.o $(APP) $(TESTS) $(TEMP_FILES)
	rm -rf $(TEMP_DIRS)
endif

.PHONY: all bench test clean
