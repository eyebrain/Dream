TARGET = Granular
CPP_SOURCES = Granular.cpp

# Library Locations
LIBDAISY_DIR = libDaisy
DAISYSP_DIR = DaisySP

# Optimize for size to fit in flash
OPT = -Oz -flto

# Core location, and generic makefile
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
