all: settings.h settings.m \
../z80/opcodes_base.c ../z80/z80_cb.c ../z80/z80_ddfd.c ../z80/z80_ddfdcb.c \
../z80/z80_ed.c settings_cocoa.h options.h options_cocoa.h options.m

settings_cocoa.h: ../settings.dat settings-cocoa-header.pl
	cd .. && perl -Iperl fusepb/settings-cocoa-header.pl settings.dat > fusepb/settings_cocoa.h

settings.h: ../settings.dat settings-header.pl
	cd .. && perl -Iperl fusepb/settings-header.pl settings.dat > fusepb/settings.h

settings.m: ../settings.dat ../settings.pl
	cd .. && perl -Iperl settings.pl --cocoa settings.dat > fusepb/settings.m

options_cocoa.h: ../perl/cpp-perl.pl config.h ../ui/cocoa/options-cocoa-header.pl ../ui/options.dat ../perl/Fuse.pm ../perl/Fuse/Dialog.pm
	cd .. && perl perl/cpp-perl.pl fusepb/config.h ui/options.dat | perl -Iperl ui/cocoa/options-cocoa-header.pl - public > fusepb/$@.tmp && mv fusepb/$@.tmp fusepb/$@

options.h: ../perl/cpp-perl.pl config.h ../ui/cocoa/options-header.pl ../ui/options.dat ../perl/Fuse.pm ../perl/Fuse/Dialog.pm
	cd .. && perl perl/cpp-perl.pl fusepb/config.h ui/options.dat | perl -Iperl ui/cocoa/options-header.pl - public > fusepb/$@.tmp && mv fusepb/$@.tmp fusepb/$@

options.m: ../perl/cpp-perl.pl config.h ../ui/cocoa/options.pl ../ui/options.dat ../perl/Fuse.pm ../perl/Fuse/Dialog.pm
	cd .. && perl perl/cpp-perl.pl fusepb/config.h ui/options.dat | perl -Iperl ui/cocoa/options.pl - > fusepb/$@.tmp && mv fusepb/$@.tmp fusepb/$@

../z80/opcodes_base.c: ../z80/opcodes_base.dat ../z80/z80.pl
	cd ../z80 && perl -I../perl z80.pl opcodes_base.dat > opcodes_base.c

../z80/z80_cb.c: ../z80/opcodes_cb.dat ../z80/z80.pl
	cd ../z80 && perl -I../perl z80.pl opcodes_cb.dat > z80_cb.c

../z80/z80_ddfd.c: ../z80/opcodes_ddfd.dat ../z80/z80.pl
	cd ../z80 && perl -I../perl z80.pl opcodes_ddfd.dat > z80_ddfd.c

../z80/z80_ddfdcb.c: ../z80/opcodes_ddfdcb.dat ../z80/z80.pl
	cd ../z80 && perl -I../perl z80.pl opcodes_ddfdcb.dat > z80_ddfdcb.c

../z80/z80_ed.c: ../z80/opcodes_ed.dat ../z80/z80.pl
	cd ../z80 && perl -I../perl z80.pl opcodes_ed.dat > z80_ed.c

clean:
	rm options.h options_cocoa.h options.m settings.h settings.m ../z80/opcodes_base.c ../z80/z80_cb.c ../z80/z80_ddfd.c ../z80/z80_ddfdcb.c ../z80/z80_ed.c settings_cocoa.h

# Generate a file-format icon, e.g. `make icon FORMAT=udi`. LABEL defaults to
# FORMAT uppercased. Writes resources/$(FORMAT).icns from resources/blank.icns.
icon:
	@test -n "$(FORMAT)" || { echo "usage: make icon FORMAT=<ext> [LABEL=<text>]"; exit 1; }
	cd .. && swift fusepb/format-icon.swift "$(FORMAT)" "$(LABEL)"
