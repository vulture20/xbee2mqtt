PROG?=xbee2mqtt

all: $(PROG)

new: clean all

clean:
	-rm $(PROG)

$(PROG): $(PROG).c
	gcc $(filter %.c,$^) -o $@ -lxbee -lpthread -lrt -lpaho-mqtt3c
