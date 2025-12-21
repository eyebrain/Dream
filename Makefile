TARGET = Granular

# Sources
CPP_SOURCES = Granular.cpp

# Library Locations
LIBDAISY_DIR = ../../libDaisy
DAISYSP_DIR = ../../DaisySP

# Optimize for size to fit in flash
OPT = -Os

# Core location, and generic makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Convenience target to build the tiny early-boot image without linking the
# full Granular example (avoids multiple `main` definitions).
.PHONY: early-boot
early-boot:
	$(MAKE) LIBDAISY_DIR=$(LIBDAISY_DIR) DAISYSP_DIR=$(DAISYSP_DIR) SYSTEM_FILES_DIR=$(LIBDAISY_DIR)/core TARGET=EarlyBoot CPP_SOURCES=EarlyBoot.cpp -f $(LIBDAISY_DIR)/core/Makefile all

.PHONY: program-dfu-early
program-dfu-early:
	$(MAKE) LIBDAISY_DIR=$(LIBDAISY_DIR) DAISYSP_DIR=$(DAISYSP_DIR) SYSTEM_FILES_DIR=$(LIBDAISY_DIR)/core TARGET=EarlyBoot CPP_SOURCES=EarlyBoot.cpp -f $(LIBDAISY_DIR)/core/Makefile program-dfu
