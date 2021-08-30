BUILD_DIR=build
RNO_G_INSTALL_DIR?=/rno-g/
PREFIX?=$(RNO_G_INSTALL_DIR)
CFLAGS=-Og -fPIC -Wall -Wextra -g -std=gnu11 -I$(RNO_G_INSTALL_DIR)/include 
BINDIR=bin


LDFLAGS=-L$(RNO_G_INSTALL_DIR)/lib
LIBS=-lz -pthread -lrno-g -lradiant -lconfig -lflower -lm -lsystemd

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
	mkdir -p $(PREFIX)/run
	chown rno-g:rno-g $(PREFIX)/run
	mkdir -p $(PREFIX)/var
	chown rno-g:rno-g $(PREFIX)/var
	mkdir -p $(PREFIX)/cfg
	chown rno-g:rno-g $(PREFIX)/cfg


install: $(BINS) setup
	install $(BINS) $(PREFIX)/bin
	install cfg/acq.cfg $(PREFIX)/cfg
	install scripts/* $(PREFIX)/bin

service-install: 
	sudo install systemd/*.service /etc/systemd/system 
	sudo systemctl daemon-reload
