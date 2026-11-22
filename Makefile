# Builds the apps here as ELF files Viper loads at run time. Each app is its
# own ESP-IDF project (a folder with a CMakeLists.txt); `idf.py elf` builds it
# against the SDK in sdk/, and sdk/tools/install_app.sh installs it as
# <apps folder>/app.viper.<app>/ with its app.elf, info.json, icon and assets.
#
#   make              build every app into out/
#   make APP=doom     build one
#   make clean
#
# On a device, apps live in apps/<bundle id>/ on the SD card (or fatfs). Copy
# the folders in out/sdcard/elf_apps/ and out/fatfs/apps/ there.
#
# Inside the Viper firmware repo (this repo at apps_public/), `make apps` there
# runs this with FATFS_APPS_DIR and SDCARD_APPS_DIR set to its own folders.

.PHONY: all clean help

BUNDLE_PREFIX := app.viper.
INSTALL := sdk/tools/install_app.sh

# Where built apps go. Apps with large assets ship on the SD card instead of
# the firmware's fatfs image; list them in SDCARD_APPS.
FATFS_APPS_DIR ?= out/fatfs/apps
SDCARD_APPS_DIR ?= out/sdcard/elf_apps
SDCARD_APPS :=

# Every folder with a CMakeLists.txt is an app project (sdk/ has none)
APPS := $(patsubst %/CMakeLists.txt,%,$(wildcard */CMakeLists.txt))
ifdef APP
APPS := $(APP)
endif

all:
	@if [ -z "$(APPS)" ]; then echo "No apps to build"; exit 0; fi
	@for app in $(APPS); do \
		if [ ! -f "$$app/CMakeLists.txt" ]; then echo "Error: no app '$$app'"; exit 1; fi; \
		echo "Building $$app..."; \
		(cd "$$app" && idf.py elf) || exit 1; \
		case " $(SDCARD_APPS) " in *" $$app "*) dest="$(SDCARD_APPS_DIR)";; *) dest="$(FATFS_APPS_DIR)";; esac; \
		mkdir -p "$$dest"; \
		$(INSTALL) "$$app" "$$dest" $(BUNDLE_PREFIX) || exit 1; \
	done

clean:
	@for app in $(APPS); do (cd "$$app" && idf.py fullclean) || true; done
	rm -rf out

help:
	@echo "make            - build every app into $(FATFS_APPS_DIR) or $(SDCARD_APPS_DIR)"
	@echo "make APP=<app>  - build one"
	@echo "make clean      - clean every app and remove out/"
	@echo "Apps: $(APPS)"
