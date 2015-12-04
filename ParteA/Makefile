obj-m +=  modlist.o 
#EXTRA_CFLAGS += -DSTRING_MODE


all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

modlist_string:
	#EXTRA_CFLAGS += -DSTRING_MODE
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

