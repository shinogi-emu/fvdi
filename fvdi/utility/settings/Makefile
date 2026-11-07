top_srcdir = ../..
include $(top_srcdir)/CONFIGVARS
TARGET = FVDISET.PRG
all: $(TARGET)
$(TARGET): settings.c config.c config.h
	$(CC) -m68000 -std=gnu99 -O2 -Wall -Wextra -Werror settings.c config.c -lgem -s -o $@
install: $(TARGET)
	mkdir -p $(DESTDIR)/TOOLS
	cp -f $(TARGET) $(DESTDIR)/TOOLS/$(TARGET)
clean:
	$(RM) $(TARGET)
