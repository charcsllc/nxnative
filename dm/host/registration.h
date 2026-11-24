// Linker-collected registrations are walked as arrays of their entry type.
#pragma once

// Every producer in a section uses the same type. Keep its natural alignment:
// stronger per-global alignment or ASan redzones would insert gaps in the array.
// Suppress only the entry's redzones; its functions and pointed-to data stay checked.
// The section is the platform layer's table of that name (plat/plat.h PLAT_TABLE_ENTRY, read back with plat_table_<name>): Mach-O __DATA,<name> on Apple, as always; an
// ELF section <name> (between __start_<name> and __stop_<name>) on Linux; the grouped PE/COFF section .<name>$m (between .<name>$a and .<name>$z) on Windows
#if defined(__APPLE__)
#define DM_REGISTER_SECTION(sect) "__DATA," sect
#elif defined(_WIN32)
#define DM_REGISTER_SECTION(sect) "." sect "$m"
#else
#define DM_REGISTER_SECTION(sect) sect
#endif
#define DM_REGISTER(sect, type, name, ...) \
    __attribute__((used, section(DM_REGISTER_SECTION(sect)), aligned(_Alignof(type)), no_sanitize("address"))) \
    static const type name = __VA_ARGS__
