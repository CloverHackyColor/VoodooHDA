CC:=xcrun clang
TARGET_ARCH:=-m64
CFLAGS:=-fvisibility=hidden -fno-stack-protector -O2 -fno-exceptions -Wall -Wextra
LDFLAGS:=-framework IOKit -Wl,-no_function_starts,-no_version_load_command,-no_data_in_code_info,-no_uuid,-no_source_version,-no_dependent_dr_info
getdump: getdump.c
	$(LINK.c) -o $@ $^
	/usr/bin/strip -x $@
