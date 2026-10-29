from marlin.rules import *


class LibClang(ThirdPartyRules):
    """The libclang C API headers, which MarlinDoc parses C++ with (Documentation/Architecture/16 §3.1).

    Only the headers are checked in, copied from an LLVM install's include/clang-c. The library itself comes from the
    LLVM install the toolkit builds with and is loaded at run time, so these headers only give its types and
    signatures; libclang's C API only grows, so they work with any newer library.
    """

    version = "23.1.4"
    url = "https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.4"
    license = "Apache-2.0 WITH LLVM-exception"
    public_include_paths = ["Include"]
