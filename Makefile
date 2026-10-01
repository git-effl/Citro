#---------------------------------------------------------------------------------
# Citro - Invidious YouTube Client for Nintendo 3DS
# Makefile configured for devkitARM / devkitPro toolchain
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB   ?= $(DEVKITPRO)/libctru

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

PREFIX ?= arm-none-eabi-
CC     := $(PREFIX)gcc
CXX    := $(PREFIX)g++
AR     := $(PREFIX)ar
OBJCOPY:= $(PREFIX)objcopy

TARGET      := Citro
BUILD       := build
SOURCES     := .
DATA        := data
INCLUDES    := include .

# 3DS Library include & library directories
LIBDIRS     := $(CTRULIB) $(DEVKITPRO)/libctru $(DEVKITPRO)/citro2d $(DEVKITPRO)/citro3d

INCLUDE     := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
               $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
               -I.

LIBPATHS    := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ARCH        := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS      := -g -Wall -O2 -mword-relocations \
               -fomit-frame-pointer -ffunction-sections \
               $(ARCH)

CFLAGS      += $(INCLUDE) -DARM11 -D_3DS

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=3ds.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) $(LIBPATHS)

# Libraries needed: citro2d, citro3d, ctru, m
LIBS        := -lcitro2d -lcitro3d -lctru -lm

-include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# 3DSX Metadata
#---------------------------------------------------------------------------------
APP_TITLE       := Citro
APP_DESCRIPTION := Invidious Client for Nintendo 3DS
APP_AUTHOR      := Citro Team
APP_PRODUCT_CODE:= CTR-P-CITR
APP_UNIQUE_ID   := 0x0CITR

.PHONY: all clean

all: $(TARGET).3dsx

$(TARGET).3dsx: $(TARGET).elf

$(TARGET).elf: main.o citro_invidious.o cJSON.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh *.o *.map
