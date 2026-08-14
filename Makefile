.PHONY: all clean

# -Wno-int-conversion: the provided main.c passes return_pages() (int) to
# PTR_ERR(const void *); newer GCC (>=14) treats that as an error by default.
CFLAGS = -O2 -Wno-int-conversion -Wno-implicit-function-declaration -Wno-implicit-int -Wno-incompatible-pointer-types

all: code

code: main.c buddy.c buddy.h utils.h
	gcc $(CFLAGS) -o code main.c buddy.c

clean:
	rm -f code
