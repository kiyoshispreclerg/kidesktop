hello.so: hello.c
	$(CC) -shared -fPIC -o $@ $< $(shell pkg-config --cflags --libs gtk+-2.0)
