#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := EPDSign
SUFFIX := $(shell components/ESP32-RevK/buildsuffix)

all:
	@echo Make: $(PROJECT_NAME)$(SUFFIX).bin
	@idf.py build
	@cp build/$(PROJECT_NAME).bin $(PROJECT_NAME)$(SUFFIX).bin
	@echo Done: $(PROJECT_NAME)$(SUFFIX).bin

issue:
	-git pull
	-git submodule update --recursive
	-git commit -a -m checkpoint
	@make set
	cp $(PROJECT_NAME)*.bin release
	git commit -a -m release
	git push

set:    epd75 epd154 eps154r

epd75:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-EPD75
	@make

epd154:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-EPD154
	@make

epd154r:
	components/ESP32-RevK/setbuildsuffix -S3-MINI-N4-R2-EPD154R
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
