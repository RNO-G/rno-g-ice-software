BUILD_DIR=build
PREFIX?=/rno-g
CFLAGS=-Og -fPIC -Wall -Wextra -g -std=gnu11 -I$(PREFIX)/include 
BINDIR=bin


LDFLAGS=-L$(PREFIX)/lib
LIBS=-lz -pthread -lrno-g -lradiant -lconfig -lflower -lm

INCLUDES=src/ice-config.h src/ice-buf.h src/ice-common.h 

.PHONY: all clean install uninstall 

OBJS:=$(addprefix $(BUILD_DIR)/, ice-config.o ice-buf.o ice-common.o)

BINS:=$(addprefix $(BINDIR)/, rno-g-acq make-default-rno-g-config check-rno-g-config ) 

all: $(OBJS) $(BINS) 

$(BINDIR)/%: src/%.c $(INCLUDES) $(OBJS) Makefile | $(BINDIR) 
	@echo Compiling $@
	@cc -o $@ $(CFLAGS) $< $(OBJS) $(LDFLAGS) $(LIBS) 

$(BUILD_DIR)/%.o: src/%.c $(INCLUDES) | $(BUILD_DIR)
	@echo Compiling $@
	@cc -c -o $@ $(CFLAGS) $< 


$(BUILD_DIR): 
	@mkdir -p $(BUILD_DIR)

$(BINDIR): 
	@mkdir -p $(BINDIR)


clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(BINDIR)

setup:
	mkdir $(PREFIX)/run
	mkdir $(PREFIX)/var

