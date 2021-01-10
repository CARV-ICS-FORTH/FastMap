default: all

all: set rmv

set: set.c def.h
	gcc set.c -o set

rmv: rm.c def.h
	gcc rm.c -o rmv

clean:
	rm -f set rmv
