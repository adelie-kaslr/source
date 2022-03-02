name = ioctrl_bench

KDIR := /lib/modules/$(shell uname -r)/build/

obj-m += $(name).o

all:
	@make -C $(KDIR) M=$(PWD) modules -j4 > /dev/null

clean:
	@make -C $(KDIR) M=$(PWD) clean

install:
	sudo cp $(name).ko /lib/modules/`uname -r`/ && sudo depmod -a

insert:
	sudo insmod $(name).ko

remove:
	sudo rmmod $(name)

run: all
	make insert && sudo user/ioctrl_bench ; make remove
