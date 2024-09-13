CC=cc
CFLAGS=-O3
AR=ar
RANLIB=ranlib
LIB=wlib

OBJS=\
	wasm-rt-impl.o \
	wasm-rt-mem-impl.o

all: libwasm-rt-impl.a

libwasm-rt-impl.a: $(OBJS)
	$(AR) rc $@ $(OBJS)
	$(RANLIB) $@

lib: $(OBJS)
	rm -f wasm-rt-impl.lib
	$(LIB) -n wasm-rt-impl.lib $(addprefix +,$(OBJS))

zcclib: $(OBJS)
	$(CC) -x -o wasm-rt-impl.lib $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f libwasm-rt-impl.a wasm-rt-impl.lib $(OBJS)
