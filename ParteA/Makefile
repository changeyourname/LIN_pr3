obj-m +=  modlist.o 
#EXTRA_CFLAGS += -DSTRING_MODE
#EXTRA_CFLAGS += -DTEST_NO_LOCK

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

modlist_string:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

