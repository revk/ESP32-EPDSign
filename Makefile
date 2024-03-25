#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := EPDSign
SUFFIX := $(shell components/ESP32-RevK/buildsuffix)

all:	settings.h
	@echo Make: $(PROJECT_NAME)$(SUFFIX).bin
	@idf.py build
	@cp build/$(PROJECT_NAME).bin $(PROJECT_NAME)$(SUFFIX).bin
	@echo Done: $(PROJECT_NAME)$(SUFFIX).bin

beta:   
	-git pull
	-git submodule update --recursive
	-git commit -a -m checkpoint
	@make set
	cp $(PROJECT_NAME)*.bin betarelease
	git commit -a -m Beta
	git push

issue:
	-git pull
	-git submodule update --recursive
	-git commit -a -m checkpoint
	@make set
	cp $(PROJECT_NAME)*.bin betarelease
	cp $(PROJECT_NAME)*.bin release
	git commit -a -m Release
	git push

settings.h:	components/ESP32-RevK/revk_settings settings.def components/ESP32-RevK/settings.def
	components/ESP32-RevK/revk_settings $^

components/ESP32-RevK/revk_settings: components/ESP32-RevK/revk_settings.c
	make -C components/ESP32-RevK

set:    epd75r epd154k epd154r ssd1681 epd29k epd75k

epd75k:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-EPD75K
	@make

epd75r:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-EPD75R
	@make

epd154k:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-EPD154K
	@make

epd154r:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-EPD154R
	@make

epd29k:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-EPD29K
	@make

ssd1681:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-SSD1681
	@make

flash:
	idf.py flash

monitor:
	idf.py monitor

clean:
	idf.py clean

menuconfig:
	idf.py menuconfig

pull:
	git pull
	git submodule update --recursive

update:
	git submodule update --init --recursive --remote
	-git commit -a -m "Library update"
