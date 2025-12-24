CC = gcc
CFLAGS = -Wall -Wextra -Werror -O3 -std=c99 $(shell pkg-config --cflags gtk4-layer-shell-0 gtk4)
LDFLAGS = $(shell pkg-config --libs gtk4-layer-shell-0 gtk4)

TARGET = calculator
SOURCE = main.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

clean:
	rm -f $(TARGET)

.PHONY: all clean install

