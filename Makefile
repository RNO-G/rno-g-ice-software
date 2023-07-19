BUILD_DIR=build
RNO_G_INSTALL_DIR?=/rno-g/
PREFIX?=$(RNO_G_INSTALL_DIR)
CFLAGS=-Og -fPIC -Wall -Wextra -g -std=gnu11 -I$(RNO_G_INSTALL_DIR)/include 
BINDIR=bin


LDFLAGS=-L$(RNO_G_INSTALL_DIR)/lib
LIBS=-lz -pthread -lrno-g -lradiant -lrno-g-cal -lconfig -lflower -lm -lsystemd

INCLUDES=src/ice-config.h src/ice-buf.h src/ice-common.h  src/ice-notify.h

.PHONY: all clean install uninstall 

OBJS:=$(addprefix $(BUILD_DIR)/, ice-config.o ice-buf.o ice-common.o ice-version.o ice-notify.o)

BINS:=$(addprefix $(BINDIR)/, rno-g-acq make-default-rno-g-config check-rno-g-config update-rno-g-config rno-g-find-config rno-g-notify) 



$(shell /bin/echo -e "/*This file is auto-generated by the Makefile!*/\n#include \"ice-version.h\"\n\nconst char *  get_ice_software_git_hash() { return \"$$(git describe --always --dirty --match 'NOT A TAG')\";}" > src/ice-version.c.tmp; if diff -q src/ice-version.c.tmp src/ice-version.c >/dev/null 2>&1; then rm src/ice-version.c.tmp; else mv src/ice-version.c.tmp src/ice-version.c; fi)

STATION_NUMBER=$(shell if [ -f /STATION_ID ] ; then printf "%03d" `cat /STATION_ID`; fi )
$(info STATION_NUMBER=${STATION_NUMBER}) 

all: $(OBJS) $(BINS) 

$(BINDIR)/update-rno-g-config:  $(BINDIR)/check-rno-g-config
	ln -f $< $@ 

$(BINDIR)/%: src/%.c $(INCLUDES) $(OBJS) Makefile | $(BINDIR) 
	@echo Compiling $@
	@cc -o $@ $(CFLAGS) $< $(OBJS) $(LDFLAGS) $(LIBS) 

$(BINDIR)/rno-g-notify: src/rno-g-notify.c $(INCLUDES) $(OBJS) Makefile | $(BINDIR) 
	@echo Compiling $@
	@cc -o $@ $(CFLAGS) $< $(OBJS) $(LDFLAGS) $(LIBS) -lcurl



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
	mkdir -p $(PREFIX)/bin
	chown rno-g:rno-g $(PREFIX)/bin
	mkdir -p /data/daq
	chown rno-g:rno-g /data/daq



install: $(BINS) setup
	install $(BINS) $(PREFIX)/bin
	install scripts/* $(PREFIX)/bin

cfg-install: 
	@ echo "Installing configuration..."
	@ if [ -f cfg/acq-${STATION_NUMBER}.cfg ] ; \
	then \
		echo "Using station-specific file cfg/acq-${STATION_NUMBER}.cfg" ; install cfg/acq-${STATION_NUMBER}.cfg $(PREFIX)/cfg/acq.cfg ;\
	else \
		echo "Using default cfg/acq.cfg" ; install cfg/acq.cfg $(PREFIX)/cfg/acq.cfg ; \
	fi
	@mkdir -p ${PREFIX}/cfg/acq.cfg.once 

cppcheck: 
	cppcheck --enable=portability --enable=performance --enable=information  src 

service-install: 
	sudo install systemd/*.service /etc/systemd/system 
	sudo systemctl daemon-reload
