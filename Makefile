DEVICE=mtmservo
KERNEL=/opt/rpi/linux
obj-m += ${DEVICE}.o

all: dt
	make ARCH=arm CROSS_COMPILE=/opt/rpi/bin/arm-linux-gnueabihf- -C $(KERNEL) M=$(shell pwd) modules

dt:
	dtc -@ -Hepapr -I dts -O dtb -o ${DEVICE}.dtbo ${DEVICE}-overlay.dts

clean:
	make -C $(KERNEL) M=$(shell pwd) clean
	rm *.dtbo
