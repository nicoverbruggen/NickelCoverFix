include NickelHook/NickelHook.mk

override LIBRARY  := src/libnickelcoverfix.so
override SOURCES  += src/config.c src/nickelcoverfix.cc
override MOCS     += src/ncfbridge.h

# QDialog/QProgressBar/QLabel (QtWidgets) + QImage/QSize/QPixmap (QtGui) + QString/QTimer (QtCore).
override PKGCONF  += Qt5Core Qt5Gui Qt5Widgets

override CFLAGS   += -Wall -Wextra -Werror -fvisibility=hidden
override CXXFLAGS += -std=gnu++11 -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden

override KOBOROOT += \
	res/doc:$(NCF_CONFIG_DIR)/doc \
	res/default:$(NCF_CONFIG_DIR)/default \
	res/uninstall:$(NCF_CONFIG_DIR)/uninstall

override SKIPCONFIGURE += strip
strip:
	$(STRIP) --strip-unneeded src/libnickelcoverfix.so
.PHONY: strip

ifeq ($(NCF_CONFIG_DIR),)
override NCF_CONFIG_DIR := /mnt/onboard/.adds/nickel-cover-fix
endif

override CPPFLAGS += -DNCF_CONFIG_DIR='"$(NCF_CONFIG_DIR)"' -DNCF_CONFIG_DIR_DISP='"$(patsubst /mnt/onboard/%,KOBOeReader/%,$(NCF_CONFIG_DIR))"'

include NickelHook/NickelHook.mk
