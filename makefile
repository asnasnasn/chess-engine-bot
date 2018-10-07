target = asnbot
cc = gcc
cflags = -Wall -Wno-deprecated-declarations -O2 # -g -DDEBUG
ldflags = -lxdo -lX11

src = main.c
obj = ${src:.c=.o}

all: $(target)

.c.o:
	@echo cc $<
	@${cc} -c ${cflags} $<

${obj}:

$(target): ${obj}
	@echo cc -o $@
	@${cc} -o $@ ${obj} ${ldflags}

clean:
	@echo cleaning
	@rm -f $(target) ${obj}

.PHONY: all clean
