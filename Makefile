CC = gcc
CFLAGS = -Wall -g -pedantic -fpic -std=gnu99
TARGET = liblwp.so
OBJS = lwp.o magic64.o
 
all: $(TARGET)
 
$(TARGET): $(OBJS)
	$(CC) -shared -o $(TARGET) $(OBJS)
 
lwp.o: lwp.c lwp.h fp.h
	$(CC) $(CFLAGS) -c lwp.c
 
magic64.o: magic64.S lwp.h fp.h
	$(CC) -c magic64.S
 
clean:
	rm -f $(TARGET) $(OBJS)