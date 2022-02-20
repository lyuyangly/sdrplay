TARGET=sdrplay
CC?=gcc
CFLAGS?=-O2 -g -Wall
LDLIBS+= -lpthread -lm -lsdrplay_api -lasound

F=97900000

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

$(TARGET): $(TARGET).o
	$(CC) -g -o $@ $^ $(LDFLAGS) $(LDLIBS)

play:
	./$(TARGET) -f $(F)

clean:
	rm -f *.o $(TARGET)

