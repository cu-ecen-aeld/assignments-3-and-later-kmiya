TARGET = aesdsocket
SRCS = aesdsocket.c
OBJS = $(SRCS:.c=.o)

CC ?= $(TARGET_CC)
CFLAGS ?= $(TARGET_CFLAGS)
CFLAGS += -Wall -Wextra -pedantic -Werror
LDFLAGS ?= $(TARGET_LDFLAGS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
