
CFLAGS = -I /usr/local/include 
LDFLAGS = -L /usr/local/lib
LIBS = -lavformat -lavcodec -lavutil -lswscale -lSDL2main -lSDL2
CC = cc
binaries = min min_sdl min_sdl_parse

all: $(binaries)

min: min.o
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

min_sdl: min_sdl.o
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

min_sdl_parse: min_sdl_parse.o
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

.PHONY: clean
clean:
	rm -f $(binaries) *.o

