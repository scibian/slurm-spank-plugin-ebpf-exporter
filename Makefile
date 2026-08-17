# Makefile for the ebpf_exporter SPANK plugin.
#
# Usage:
#   make                # build spank_ebpf.so
#   make install         # install the plugin and the plugstack.conf.d snippet
#   make uninstall       # remove them
#   make clean           # remove build artifacts
#
# Run `make install` on both compute nodes and submission hosts (login,
# controller): opt-in mode needs the plugin loaded wherever sbatch/srun/salloc
# run, see README.md.

CC       ?= gcc
CFLAGS   ?= -Wall -fPIC
LDFLAGS  ?= -shared

TARGET   := spank_ebpf.so
SRC      := spank_ebpf.c

SLURM_LIBDIR      ?= /usr/lib64/slurm
PLUGSTACK_CONF_DIR ?= /etc/slurm/plugstack.conf.d
PLUGSTACK_CONF     := ebpf.plugstack.conf

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: $(TARGET)
	install -d $(DESTDIR)$(SLURM_LIBDIR)
	install -m0755 $(TARGET) $(DESTDIR)$(SLURM_LIBDIR)/$(TARGET)
	install -d $(DESTDIR)$(PLUGSTACK_CONF_DIR)
	install -m0644 $(PLUGSTACK_CONF) $(DESTDIR)$(PLUGSTACK_CONF_DIR)/ebpf.conf

uninstall:
	rm -f $(DESTDIR)$(SLURM_LIBDIR)/$(TARGET)
	rm -f $(DESTDIR)$(PLUGSTACK_CONF_DIR)/ebpf.conf

clean:
	rm -f $(TARGET)
