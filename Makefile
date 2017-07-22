SRCS := $(wildcard src/*.c)
HDRS := $(wildcard src/*.h)
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

CFLAGS := -g -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-pointer-sign\
 -Wno-unused-parameter -Wno-missing-field-initializers

morpheus: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ -lcurl -lyajl

build:
	mkdir $@

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@
	
clean:
	$(RM) $(OBJS) morpheus

.PHONY: clean
