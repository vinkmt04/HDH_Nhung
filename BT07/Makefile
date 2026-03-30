obj-m += my_driver.o

KDIR = /home/vinh/buildroot_hdh/output/build/linux-6.6.121

all:
	make ARCH=arm CROSS_COMPILE=/home/vinh/buildroot_hdh/output/host/bin/arm-buildroot-linux-gnueabihf- -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
