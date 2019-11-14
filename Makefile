all: server.c
	gcc -g server.c -o a.out
clean: 
	rm a.out