CC = clang
PORT=58868
CFLAGS = -DPORT=\$(PORT) -g -Wall -Werror -Wextra 

SRCS = battle.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: battle

battle: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o battle

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) battle
