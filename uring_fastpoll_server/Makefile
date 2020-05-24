all: build

clean:
	rm ur_server

build:
	gcc ur_server.c -o ./ur_server -I./liburing/src/include/ -L./liburing/src/ -Wall -O2 -D_GNU_SOURCE -pthread -luring

