TARGET = aesdsocket
SRCS = aesdsocket.c
OBJS = $(SRCS:.c=.o)

CC ?= $(TARGET_CC)
CFLAGS ?= $(TARGET_CFLAGS)
CFLAGS += -Wall -Wextra -pedantic -Werror -O1
LDFLAGS ?= $(TARGET_LDFLAGS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

asan: CFLAGS += -fsanitize=address -fno-omit-frame-pointer -g
asan: LDFLAGS += -fsanitize=address
asan: clean $(TARGET)

ubsan: CFLAGS += -fsanitize=undefined -fno-omit-frame-pointer -g
ubsan: LDFLAGS += -fsanitize=undefined
ubsan: clean $(TARGET)

.PHONY: all clean asan ubsan msan
