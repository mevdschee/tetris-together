CFLAGS ?= -std=c99 -O2 -Wall -Wextra

tetris-together: tetris-together.c
	$(CC) $(CFLAGS) -o $@ $< -lm

run: tetris-together
	./tetris-together --bots 2

clean:
	rm -f tetris-together

.PHONY: run clean
