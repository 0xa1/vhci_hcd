HCD_TARGET = usb-vhci-hcd
IOCIFC_TARGET = usb-vhci-iocifc
OBJS = $(HCD_TARGET).o $(IOCIFC_TARGET).o
MDIR = drivers/usb/host

PREFIX =
BUILD_PREFIX = $(PREFIX)
INSTALL_PREFIX = $(PREFIX)
EXTRA_CFLAGS = -DEXPORT_SYMTAB -DKBUILD_EXTMOD -DINCLUDE_CORE_HCD=\"$(CORE_INCLUDE_DIR)/hcd.h\"
OLD_CORE_INCLUDE_DIR = $(KDIR)/drivers/usb/core
ORIG_CORE_INCLUDE_DIR = $(KDIR)/include/linux/usb
COPY_CORE_INCLUDE_DIR = $(PWD)/linux/$(KVERSION_VERSION).$(KVERSION_PATCHLEVEL).$(KVERSION_SUBLEVEL)/drivers/usb/core
CORE_INCLUDE_DIR = $(shell test -e $(ORIG_CORE_INCLUDE_DIR)/hcd.h && echo $(ORIG_CORE_INCLUDE_DIR) || (test -e $(OLD_CORE_INCLUDE_DIR)/hcd.h && echo $(OLD_CORE_INCLUDE_DIR) || echo $(COPY_CORE_INCLUDE_DIR)))
KVERSION = $(shell uname -r)
KVERSION_VERSION = $(shell echo $(KVERSION) | ( awk -F - '{ ORS = ""; print $$1 }'; echo '.0.0.0' ) | awk -F . '{ print $$1 }')
KVERSION_PATCHLEVEL = $(shell echo $(KVERSION) | ( awk -F - '{ ORS = ""; print $$1 }'; echo '.0.0.0' ) | awk -F . '{ print $$2 }')
KVERSION_SUBLEVEL = $(shell echo $(KVERSION) | ( awk -F - '{ ORS = ""; print $$1 }'; echo '.0.0.0' ) | awk -F . '{ print $$3 }')
KDIR = $(BUILD_PREFIX)/lib/modules/$(KVERSION)/build
PWD = $(shell pwd)
INSTALL_DIR = $(INSTALL_PREFIX)/lib/modules/$(KVERSION)
DEST = $(INSTALL_DIR)/kernel/$(MDIR)
KSRC = $(KDIR)

CONF_H = conf/usb-vhci.config.h

VHCI_HCD_VERSION = 1.14
USB_VHCI_HCD_VERSION = $(VHCI_HCD_VERSION)
USB_VHCI_IOCIFC_VERSION = $(VHCI_HCD_VERSION)
DIST_DIRS = patch test
DIST_FILES = AUTHORS ChangeLog COPYING INSTALL Makefile NEWS README TODO usb-vhci-hcd.c usb-vhci-iocifc.c usb-vhci-hcd.h usb-vhci.h usb-vhci-dump-urb.c patch/Kconfig.patch test/Makefile test/test.c

obj-m := $(OBJS)

default: $(CONF_H)
	make -C $(KDIR) SUBDIRS=$(PWD) PWD=$(PWD) BUILD_PREFIX=$(BUILD_PREFIX) KDIR=$(KDIR) KVERSION=$(KVERSION) modules
.PHONY: default
.SUFFIXES:

ifneq (,$(INSTALL_PREFIX))
install-module:
	mkdir -v -p $(DEST) && cp -v $(HCD_TARGET).ko $(IOCIFC_TARGET).ko $(DEST) && /sbin/depmod -a -b $(INSTALL_PREFIX) $(KVERSION)
else
install-module:
	mkdir -v -p $(DEST) && cp -v $(HCD_TARGET).ko $(IOCIFC_TARGET).ko $(DEST) && /sbin/depmod -a $(KVERSION)
endif
.PHONY: install-module

install-devel:
	mkdir -v -p $(INSTALL_PREFIX)/usr/include/linux/ && cp -v -p usb-vhci.h $(INSTALL_PREFIX)/usr/include/linux/
.PHONY: install-devel

install: install-module install-devel
.PHONY: install

clean-conf:
	-rm -f $(CONF_H)
	-rmdir conf/
.PHONY: clean-conf

clean: clean-test clean-conf
	-rm -f *.o *.ko .*.cmd .*.flags *.mod.c Module.symvers Module.markers modules.order
	-rm -rf .tmp_versions/
	-rm -rf $(TMP_MKDIST_ROOT)/
.PHONY: clean

patchkernel: $(CONF_H)
	cp -v usb-vhci-hcd.{c,h} usb-vhci-iocifc.c usb-vhci-dump-urb.c $(CONF_H) $(KSRC)/$(MDIR)/
	cp -v usb-vhci.h $(KSRC)/include/linux/
	cd $(KSRC)/$(MDIR); grep -q $(HCD_TARGET).o Makefile || echo "obj-\$$(CONFIG_USB_VHCI_HCD)	+= $(HCD_TARGET).o" >>Makefile
	cd $(KSRC)/$(MDIR); grep -q $(IOCIFC_TARGET).o Makefile || echo "obj-\$$(CONFIG_USB_VHCI_IOCIFC)	+= $(IOCIFC_TARGET).o" >>Makefile
	cd $(KSRC)/$(MDIR)/..; grep -q CONFIG_USB_VHCI_HCD Makefile || echo "obj-\$$(CONFIG_USB_VHCI_HCD)	+= host/" >>Makefile
	cd $(KSRC)/$(MDIR); patch -N -i $(PWD)/patch/Kconfig.patch || :
	if [ "$(KVERSION_VERSION)" -eq 2 -a "$(KVERSION_PATCHLEVEL)" -eq 6 -a "$(KVERSION_SUBLEVEL)" -lt 35 ]; then \
		sed -i -e 's,<linux/usb/hcd.h>,"../core/hcd.h",' $(KSRC)/$(MDIR)/usb-vhci-hcd.h; \
	fi
.PHONY: patchkernel

clean-srcdox:
	-rm -rf html/ vhci-hcd.tag
.PHONY: clean-srcdox

srcdox: clean-srcdox
	mkdir -p html/
	doxygen
	-rm -f vhci-hcd.tag
.PHONY: srcdox

$(CONF_H):
	$(MAKE) testconfig

TESTMAKE = make -C $(KDIR) SUBDIRS=$(PWD)/test PWD=$(PWD)/test BUILD_PREFIX=$(BUILD_PREFIX) KDIR=$(KDIR) KVERSION=$(KVERSION) EXTRA_CFLAGS='-Wno-unused $(EXTRA_CFLAGS) $1' modules

testcc: clean-test
	$(call TESTMAKE)
.PHONY: testcc

clean-test:
	-rm -f test/*.o test/*.ko test/.*.cmd test/.*.flags test/*.mod.c test/Module.symvers test/Module.markers test/modules.order
	-rm -rf test/.tmp_versions/
.PHONY: clean-test

testconfig: testcc
	mkdir -p conf/
	echo "// do not edit; automatically generated by 'make testconfig' in vhci-hcd sourcedir" >$(CONF_H)
	echo "#define USB_VHCI_HCD_VERSION \"$(USB_VHCI_HCD_VERSION)\"" >>$(CONF_H)
	echo "#define USB_VHCI_HCD_DATE \"$(shell date +"%F")\"" >>$(CONF_H)
	echo "#define USB_VHCI_IOCIFC_VERSION \"$(USB_VHCI_IOCIFC_VERSION)\"" >>$(CONF_H)
	echo "#define USB_VHCI_IOCIFC_DATE USB_VHCI_HCD_DATE" >>$(CONF_H)
	$(MAKE) clean-test
	if $(call TESTMAKE,-DTEST_GIVEBACK_MECH) >/dev/null 2>&1; then \
		echo "//#define OLD_GIVEBACK_MECH" >>$(CONF_H); \
	else \
		echo "#define OLD_GIVEBACK_MECH" >>$(CONF_H); \
	fi
	$(MAKE) clean-test
	if $(call TESTMAKE,-DTEST_DEV_BUS_ID) >/dev/null 2>&1; then \
		echo "//#define OLD_DEV_BUS_ID" >>$(CONF_H); \
	else \
		echo "#define OLD_DEV_BUS_ID" >>$(CONF_H); \
	fi
	$(MAKE) clean-test
	if $(call TESTMAKE,-DTEST_DEV_INIT_NAME) >/dev/null 2>&1; then \
		echo "//#define NO_DEV_INIT_NAME" >>$(CONF_H); \
	else \
		echo "#define NO_DEV_INIT_NAME" >>$(CONF_H); \
	fi
	$(MAKE) clean-test
	if $(call TESTMAKE,-DTEST_HAS_TT_FLAG) >/dev/null 2>&1; then \
		echo "//#define NO_HAS_TT_FLAG" >>$(CONF_H); \
	else \
		echo "#define NO_HAS_TT_FLAG" >>$(CONF_H); \
	fi
	echo "// end of file" >>$(CONF_H)
.PHONY: testconfig

config:
	@echo "**********************************************************"; \
	echo " Please answer the following questions." ; \
	echo " Your answers will influence the creation of $(CONF_H)"; \
	echo " which is needed to build this vhci-hcd driver."; \
	echo "**********************************************************"; \
	echo; \
	echo "NOTE: You can let me do this for you automatically without answering this"; \
	echo "      questions by running 'make testconfig'. I will compile a few test modules"; \
	echo "      for the target kernel which helps me guessing the answers. So if this"; \
	echo "      is not possible on the currently running system and you just want to"; \
	echo "      patch the sources into a kernel source tree, then answering them by"; \
	echo "      yourself is the right thing to do."; \
	echo; \
	echo "NOTE: You can cancel this at any time (by pressing CTRL-C). $(CONF_H)"; \
	echo "      will not be overwritten then."; \
	echo; \
	echo "Question 1 of 4:"; \
	echo "  What does the signature of usb_hcd_giveback_urb look like?"; \
	echo "   a) usb_hcd_giveback_urb(struct usb_hcd *, struct urb *, int)    <-- recent kernels"; \
	echo "   b) usb_hcd_giveback_urb(struct usb_hcd *, struct urb *)         <-- older kernels"; \
	echo "  You may find it in <KERNEL_SRCDIR>/drivers/usb/core/hcd.h."; \
	OLD_GIVEBACK_MECH=; \
	while true; do \
		echo -n "Answer (a/b): "; \
		read ANSWER; \
		if [ "$$ANSWER" = a ]; then break; \
		elif [ "$$ANSWER" = b ]; then \
			OLD_GIVEBACK_MECH=y; \
			break; \
		fi; \
	done; \
	echo; \
	echo "Question 2 of 4:"; \
	echo "  Are the functions dev_name and dev_set_name defined?"; \
	echo "  You may find them in <KERNEL_SRCDIR>/include/linux/device.h."; \
	OLD_DEV_BUS_ID=; \
	while true; do \
		echo -n "Answer (y/n): "; \
		read ANSWER; \
		if [ "$$ANSWER" = y ]; then break; \
		elif [ "$$ANSWER" = n ]; then \
			OLD_DEV_BUS_ID=y; \
			break; \
		fi; \
	done; \
	echo; \
	echo "Question 3 of 4:"; \
	echo "  Does the device structure has the init_name field?"; \
	echo "  You may check <KERNEL_SRCDIR>/include/linux/device.h to find out."; \
	echo "  It is always safe to answer 'n'."; \
	NO_DEV_INIT_NAME=; \
	while true; do \
		echo -n "Answer (y/n): "; \
		read ANSWER; \
		if [ "$$ANSWER" = y ]; then break; \
		elif [ "$$ANSWER" = n ]; then \
			NO_DEV_INIT_NAME=y; \
			break; \
		fi; \
	done; \
	echo; \
	echo "Question 4 of 4:"; \
	echo "  Does the usb_hcd structure has the has_tt field?"; \
	echo "  This field was added in kernel version 2.6.35."; \
	NO_HAS_TT_FLAG=; \
	while true; do \
		echo -n "Answer (y/n): "; \
		read ANSWER; \
		if [ "$$ANSWER" = y ]; then break; \
		elif [ "$$ANSWER" = n ]; then \
			NO_HAS_TT_FLAG=y; \
			break; \
		fi; \
	done; \
	echo; \
	echo "Thank you"; \
	mkdir -p conf/; \
	echo "// do not edit; automatically generated by 'make config' in vhci-hcd sourcedir" >$(CONF_H); \
	echo "#define USB_VHCI_HCD_VERSION \"$(USB_VHCI_HCD_VERSION)\"" >>$(CONF_H); \
	echo "#define USB_VHCI_HCD_DATE \"$(shell date +"%F")\"" >>$(CONF_H); \
	echo "#define USB_VHCI_IOCIFC_VERSION \"$(USB_VHCI_IOCIFC_VERSION)\"" >>$(CONF_H); \
	echo "#define USB_VHCI_IOCIFC_DATE USB_VHCI_HCD_DATE" >>$(CONF_H); \
	if [ -z "$$OLD_GIVEBACK_MECH" ]; then \
		echo "//#define OLD_GIVEBACK_MECH" >>$(CONF_H); \
	else \
		echo "#define OLD_GIVEBACK_MECH" >>$(CONF_H); \
	fi; \
	if [ -z "$$OLD_DEV_BUS_ID" ]; then \
		echo "//#define OLD_DEV_BUS_ID" >>$(CONF_H); \
	else \
		echo "#define OLD_DEV_BUS_ID" >>$(CONF_H); \
	fi; \
	if [ -z "$$NO_DEV_INIT_NAME" ]; then \
		echo "//#define NO_DEV_INIT_NAME" >>$(CONF_H); \
	else \
		echo "#define NO_DEV_INIT_NAME" >>$(CONF_H); \
	fi; \
	if [ -z "$$NO_HAS_TT_FLAG" ]; then \
		echo "//#define NO_HAS_TT_FLAG" >>$(CONF_H); \
	else \
		echo "#define NO_HAS_TT_FLAG" >>$(CONF_H); \
	fi; \
	echo "// end of file" >>$(CONF_H)
.PHONY: config

TMP_MKDIST_ROOT = .tmp_make_dist
TMP_MKDIST = $(TMP_MKDIST_ROOT)/vhci-hcd-$(VHCI_HCD_VERSION)

dist:
	-rm -rf $(TMP_MKDIST_ROOT)/
	mkdir -p $(TMP_MKDIST)
	$(foreach x,$(DIST_DIRS),mkdir -p $(TMP_MKDIST)/$(x);)
	$(foreach x,$(DIST_FILES),cp -p $(x) $(TMP_MKDIST)/$(x);)
	cp -p -R linux/ $(TMP_MKDIST)/
	(cd $(TMP_MKDIST_ROOT)/; tar -c vhci-hcd-$(VHCI_HCD_VERSION)) | bzip2 -cz9 >vhci-hcd-$(VHCI_HCD_VERSION).tar.bz2
	(cd $(TMP_MKDIST_ROOT)/; tar -c vhci-hcd-$(VHCI_HCD_VERSION)) | gzip -c >vhci-hcd-$(VHCI_HCD_VERSION).tar.gz
	-rm -rf $(TMP_MKDIST_ROOT)/
.PHONY: dist

-include $(KDIR)/Rules.make
