mingw32_CC=clang -target $(host) -B$(build_prefix)/bin
mingw32_CXX=clang++ -target $(host) -B$(build_prefix)/bin -stdlib=libc++

mingw32_CFLAGS=-pipe
mingw32_CXXFLAGS=$(mingw32_CFLAGS) -isystem $(host_prefix)/include/c++/v1

mingw32_LDFLAGS=
mingw32_LDFLAGS+=-L/usr/lib/gcc/x86_64-w64-mingw32/$(shell x86_64-w64-mingw32-g++-posix -dumpversion)

mingw32_release_CFLAGS=-O3
mingw32_release_CXXFLAGS=$(mingw32_release_CFLAGS)

mingw32_debug_CFLAGS=-O0
mingw32_debug_CXXFLAGS=$(mingw32_debug_CFLAGS)

mingw32_debug_CPPFLAGS=-D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC

mingw32_cmake_system=Windows
x86_64_mingw32_native_toolchain=native_clang