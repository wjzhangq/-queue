target=queue
CFLAGS=-Wall
LDFLAGS=-lpthread
$(target): $(target).c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)
.PHONY: clean
clean:
	-rm -f $(target)
