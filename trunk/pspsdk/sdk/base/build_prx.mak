# PSP Software Development Kit - http://www.pspdev.org
# -----------------------------------------------------------------------
# Licensed under the BSD license, see LICENSE in PSPSDK root for details.
#
# build.mak - Base makefile for projects using PSPSDK.
#
# Copyright (c) 2005 Marcus R. Brown <mrbrown@ocgnet.org>
# Copyright (c) 2005 James Forshaw <tyranid@gmail.com>
# Copyright (c) 2005 John Kelley <ps2dev@kelley.ca>
#
# $Id: build.mak 771 2005-07-24 10:43:54Z tyranid $

# Note: The PSPSDK make variable must be defined before this file is included.
ifeq ($(PSPSDK),)
$(error $$(PSPSDK) is undefined.  Use "PSPSDK := $$(shell psp-config --pspsdk-path)" in your Makefile)
endif

CC       = psp-gcc
CXX      = psp-g++
AS       = psp-gcc
LD       = psp-gcc

# Add in PSPSDK includes and libraries.
INCDIR   := $(INCDIR) . $(PSPSDK)/include
LIBDIR   := $(LIBDIR) . $(PSPSDK)/lib

CFLAGS   := $(addprefix -I,$(INCDIR)) $(CFLAGS)
CXXFLAGS := $(CFLAGS) $(CXXFLAGS)
ASFLAGS  := $(CFLAGS) $(ASFLAGS)

LDFLAGS  := $(addprefix -L,$(LIBDIR)) -Wl,-q,-T$(PSPSDK)/lib/linkfile.prx -mno-crt0 -nostartfiles $(LDFLAGS)

# Library selection.  By default we link with Newlib's libc.  Allow the
# user to link with PSPSDK's libc if USE_PSPSDK_LIBC is set to 1.
PSPSDK_LIBC_LIB = -lc
ifeq ($(USE_PSPSDK_LIBC),1)
PSPSDK_LIBC_LIB = -lpsplibc
endif

# Link with following default libraries.  Other libraries should be specified in the $(LIBS) variable.
# TODO: This library list needs to be generated at configure time.
PSPSDK_LIBS = -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpspsdk
LIBS     := $(LIBS) $(PSPSDK_LIBS) $(PSPSDK_LIBC_LIB) -lpspuser -lpspkernel

FINAL_TARGET = $(TARGET).prx

all: $(FINAL_TARGET)

$(TARGET).elf: $(OBJS)
	$(LINK.c) $^ $(LIBS) -o $@

%.prx: %.elf
	psp-prxgen $< $@

%.c: %.exp
	psp-build-exports -b $< > $@

clean: $(EXTRA_CLEAN)
	-rm -f $(FINAL_TARGET) $(TARGET).elf $(OBJS)
