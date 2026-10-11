#!/usr/bin/env python3
import os
import sys

SRC = sys.argv[1]


def path(rel):
    return os.path.join(SRC, rel)


def replace_once(rel, old, new):
    p = path(rel)
    s = open(p).read()
    n = s.count(old)
    if n != 1:
        sys.exit("%s: anchor found %d times, expected 1:\n%r" % (rel, n, old[:80]))
    open(p, "w").write(s.replace(old, new, 1))
    print("patched " + rel)


def insert_before_once(rel, anchor, text):
    lead = "\n" if anchor.startswith("\n") else ""
    body = anchor[len(lead):]
    replace_once(rel, anchor, lead + text + body)


replace_once("config.sub",
             "\t     | fiwix* )\n",
             "\t     | fiwix* | nyonos* )\n")

insert_before_once("gcc/config.gcc",
                   "\n*-*-freebsd*)\n",
                   '*-*-nyonos*)\n'
                   '  extra_options="$extra_options gnu-user.opt"\n'
                   '  gas=yes\n'
                   '  gnu_ld=yes\n'
                   '  default_use_cxa_atexit=yes\n'
                   '  use_gcc_stdint=provide\n'
                   '  ;;\n')
insert_before_once("gcc/config.gcc",
                   "x86_64-*-linux* | x86_64-*-kfreebsd*-gnu)\n",
                   'x86_64-*-nyonos*)\n'
                   '\ttm_file="${tm_file} i386/unix.h i386/att.h dbxelf.h elfos.h '
                   'glibc-stdint.h i386/x86-64.h nyonos.h gnu-user.h"\n'
                   '\ttmake_file="${tmake_file} i386/t-linux64"\n'
                   '\t;;\n')

NYONOS_H = r'''/* nyonOS target configuration.  x86_64 ELF, hosted, libc = mlibc.  */

#undef LIB_SPEC
#define LIB_SPEC "-lc"

#undef STARTFILE_SPEC
#define STARTFILE_SPEC "crt1.o%s crti.o%s crtbegin.o%s"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "crtend.o%s crtn.o%s"

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()      \
  do {                                \
    builtin_define ("__nyonos__");    \
    builtin_define ("__unix__");      \
    builtin_assert ("system=nyonos"); \
    builtin_assert ("system=unix");   \
    builtin_assert ("system=posix");  \
  } while (0);
'''
with open(path("gcc/config/nyonos.h"), "w") as f:
    f.write(NYONOS_H)
print("wrote gcc/config/nyonos.h")

replace_once("libgcc/config.host",
             "\n*-*-fuchsia*)\n",
             "\nx86_64-*-nyonos*)\n"
             "\textra_parts=\"$extra_parts crti.o crtbegin.o crtend.o crtn.o\"\n"
             "\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"\n"
             "\t;;\n"
             "*-*-fuchsia*)\n")

replace_once("fixincludes/mkfixinc.sh",
             "#! /bin/sh\n",
             "#! /bin/sh\n"
             "# nyonOS: fixincludes disabled.  Generate a no-op fixinc.sh.\n"
             "cat > fixinc.sh <<'NYONOS_EOF'\n"
             "#!/bin/sh\n"
             "exit 0\n"
             "NYONOS_EOF\n"
             "chmod +x fixinc.sh\n"
             "exit 0\n")

replace_once("libcody/cody.hh",
             "constexpr char S2C (char const (&s)[I])\n{\n  static_assert (I == 2, \"only single octet strings may be converted\");\n  return s[0];\n}\n",
             "constexpr char S2C (char const (&s)[I])\n{\n  static_assert (I == 2, \"only single octet strings may be converted\");\n  return s[0];\n}\n\n"
             "#if __cplusplus >= 202002L\n"
             "template<unsigned I>\n"
             "constexpr char S2C (char8_t const (&s)[I])\n{\n"
             "  static_assert (I == 2, \"only single octet strings may be converted\");\n"
             "  return static_cast<char> (s[0]);\n}\n"
             "#endif\n")

replace_once("libcody/Makefile.in",
             "CXXOPTS := $(CXXFLAGS) @PICFLAG@\n",
             "CXXOPTS := $(CXXFLAGS) @PICFLAG@ -std=c++17\n")

replace_once("gcc/system.h",
             '#include "safe-ctype.h"\n\n#include <sys/types.h>\n',
             '#include "safe-ctype.h"\n\n'
             "#ifdef __cplusplus\n"
             "#undef isalpha\n#undef isalnum\n#undef iscntrl\n#undef isdigit\n"
             "#undef isgraph\n#undef islower\n#undef isprint\n#undef ispunct\n"
             "#undef isspace\n#undef isupper\n#undef isxdigit\n"
             "#undef toupper\n#undef tolower\n"
             "#endif\n\n#include <sys/types.h>\n")

print("all patches applied")
