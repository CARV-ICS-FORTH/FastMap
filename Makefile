SUBDIRS	= driver ioctl tests logbench

all: $(SUBDIRS)
	sync && sync && sync

$(SUBDIRS):
	$(MAKE) -C $(@)

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir $(@); \
	done

.PHONY: $(SUBDIRS) clean
