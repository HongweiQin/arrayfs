#
# A shared Makefile from "https://www.kernel.org/doc/Documentation/kbuild/modules.txt"
# and optimized by myself
#

modname ?= arrayfs
sourcelist ?= super.o 
#headerdir ?=

#==========================================================
ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
obj-m  := $(modname).o
#ccflags-y := -I$(headerdir)
$(modname)-y := $(sourcelist)

else
# normal makefile
KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD
	rm -rf modules.order .tmp_versions *.mod* *.o *.o.cmd .*.cmd
clean:
	rm -rf modules.order Module.symvers .tmp_versions *.ko* *.mod* *.o *.o.cmd .*.cmd 

#Module specific targets
hello:
	echo "hello"
endif
