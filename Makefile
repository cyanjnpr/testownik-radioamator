CC = gcc
CFLAGS := -Wall -std=c17 $(shell pkg-config --cflags poppler-glib)
LDFLAGS := $(shell pkg-config --libs poppler-glib) -lm -larchive

TARGET = exam
SRC = exam.c
OBJS = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c %.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
