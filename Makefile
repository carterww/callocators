CC = gcc
CFLAGS = -I.

example_page: build_dir bin_dir 
	$(CC) $(CFLAGS) -g -o bin/$@ page.c examples/ex_page.c

build_dir:
	mkdir -p build

bin_dir:
	mkdir -p bin
