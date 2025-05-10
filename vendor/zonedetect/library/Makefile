#Copyright (c) 2018, Bertold Van den Bergh
#All rights reserved.
#
#Redistribution and use in source and binary forms, with or without
#modification, are permitted provided that the following conditions are met:
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in the
#      documentation and/or other materials provided with the distribution.
#    * Neither the name of the author nor the
#      names of its contributors may be used to endorse or promote products
#      derived from this software without specific prior written permission.
#
#THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
#ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
#WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
#DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTOR BE LIABLE FOR ANY
#DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
#(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
#LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
#ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
#SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# https://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html
VERSION = 0.0.0

CC?=$(CCARCH)gcc
STRIP?=$(CCARCH)strip

CFLAGS=$(if $(DEBUG),-O0 -g,-O3) -std=gnu99 -pedantic -Wall -Wextra -Wconversion -Werror -c -fmessage-length=0 -ffunction-sections -fdata-sections
LDFLAGS=-shared

ifeq ($(OS),Windows_NT)
  EXT=dll
else
  EXT=so
  VER_MAJ = $(word 1,$(subst ., ,$(VERSION)))
  VER_MIN = $(word 2,$(subst ., ,$(VERSION)))
  CFLAGS += -fPIC
  LDFLAGS += -Wl,-soname=$(EXECUTABLE).$(VERSION) -Wl,--hash-style=gnu
endif

prefix ?= /usr
libdir ?= $(prefix)/lib
includedir ?= $(prefix)/include

EXECUTABLE=libzonedetect.$(EXT)
INCLUDES_SRC=zonedetect.h
SOURCES_SRC=zonedetect.c


OBJECTS_OBJ=$(SOURCES_SRC:.c=.o)

.PHONY:	all install clean nice

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS_OBJ)
	$(CC) $(LDFLAGS) $(OBJECTS_OBJ) -o $@

obj/%.o: src/%.c $(INCLUDES_SRC)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(OBJECTS_OBJ) $(EXECUTABLE)

install: $(EXECUTABLE)
	install -m 0755 -d $(DESTDIR)$(includedir) $(DESTDIR)$(libdir)
	install -m 0644 -t $(DESTDIR)$(includedir) zonedetect.h
ifeq ($(OS),Windows_NT)
	install -m 0644 $(EXECUTABLE) -t $(DESTDIR)$(libdir) $(if $(STRIP),--strip --strip-program=$(STRIP))
else
	install -m 0644 $(EXECUTABLE) -D $(DESTDIR)$(libdir)/$(EXECUTABLE).$(VERSION) $(if $(STRIP),--strip --strip-program=$(STRIP))
	ln -sf $(EXECUTABLE).$(VERSION) $(DESTDIR)$(libdir)/$(EXECUTABLE).$(VER_MAJ).$(VER_MIN)
	ln -sf $(EXECUTABLE).$(VER_MAJ).$(VER_MIN) $(DESTDIR)$(libdir)/$(EXECUTABLE).$(VER_MAJ)
	ln -sf $(EXECUTABLE).$(VER_MAJ) $(DESTDIR)$(libdir)/$(EXECUTABLE)
endif
	# Assuming DESTDIR is set for cross-installing only, we don't need
	# (and probably do not have) ldconfig
	$(if $(DESTDIR),,ldconfig)

nice:
	mkdir -p bak/
	touch $(addsuffix .orig,$(INCLUDES_SRC))
	touch $(addsuffix .orig,$(SOURCES_SRC))
	astyle --style=k/r --indent=spaces=4 --indent-cases --indent-switches  $(INCLUDES_SRC) $(SOURCES_SRC)
	mv $(addsuffix .orig,$(INCLUDES_SRC)) bak/
	mv $(addsuffix .orig,$(SOURCES_SRC)) bak/
