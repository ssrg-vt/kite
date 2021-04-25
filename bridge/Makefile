BRANCH = -netbsd10
CONF   = hw_netback

BIN    = ../rumprun$(BRANCH)/bin
CFLAGS = -Wall -O0 -g
LIBS   = -DPORTMAP -DRUMPRUN -lprop -lutil -lm
TUPLE  = obj-amd64-hw
CC = ../$(TUPLE)$(BRANCH)/app-tools/x86_64-rumprun-netbsd-gcc 

IFCONFIG_SRC = $(wildcard ifconfig_src/*.c)
BRCONFIG_SRC = $(wildcard brconfig_src/*.c)

bridge: $(IFCONFIG_SRC) $(BRCONFIG_SRC) main.c
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

bake_bridge: bridge
	$(BIN)/rumprun-bake $(CONF) back.bin bridge

create_iso: bake_bridge
	$(shell rm rumprun-back.bin.iso)
	$(BIN)/rumprun iso back.bin

run:
	xl create -c back.conf

all: clean create_iso run

clean:
	$(shell rm bridge back.bin rumprun-back.bin.iso)
