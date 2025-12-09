package=bdb
$(package)_version=6.2.23
$(package)_download_path=https://download.oracle.com/berkeley-db
$(package)_file_name=db-$($(package)_version).tar.gz
$(package)_sha256_hash=47612c8991aa9ac2f6be721267c8d3cdccf5ac83105df8e50809daea24e95dc7
$(package)_build_subdir=build_unix
$(package)_patches=clang-12-stpcpy-issue.diff winioctl-and-atomic_init_db.patch

ifneq ($(host_os),darwin)
$(package)_dependencies=libcxx
endif

define $(package)_set_vars
$(package)_config_opts=--disable-shared --enable-cxx --disable-replication --enable-option-checking
$(package)_config_opts_mingw32=--enable-mingw
$(package)_config_opts_linux=--with-pic
$(package)_config_opts_freebsd=--with-pic
$(package)_config_opts_darwin=--with-mutex=Darwin/_spin_lock_try
# Disable atomic support when cross-compiling for Darwin (test program can't run)
ifneq ($(build_os),darwin)
$(package)_config_opts_darwin+=--disable-atomicsupport
endif
ifneq ($(host_os),darwin)
$(package)_config_opts_aarch64=--disable-atomicsupport
endif
$(package)_cxxflags+=-std=c++17
$(package)_cflags+=-Wno-deprecated-non-prototype

$(package)_ldflags+=-static-libstdc++ -Wno-unused-command-line-argument
ifeq ($(host_os),freebsd)
  $(package)_ldflags+=-lcxxrt
else ifeq ($(host_os),mingw32)
  $(package)_ldflags+=
else
  $(package)_ldflags+=-lc++abi
endif

endef

define $(package)_preprocess_cmds
  patch -p1 <$($(package)_patch_dir)/clang-12-stpcpy-issue.diff && \
  patch -p1 <$($(package)_patch_dir)/winioctl-and-atomic_init_db.patch
endef

define $(package)_config_cmds
  ../dist/$($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE) libdb_cxx-6.2.a libdb-6.2.a
endef

ifeq ($(host_os),mingw32)
# For Windows, only install libraries and headers (utilities don't build properly)
define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install_lib install_include
endef
else ifneq ($(build_os),darwin)
# Install the BDB utilities as well, so that we have the specific compatible
# versions for recovery purposes (https://github.com/zcash/zcash/issues/4537).
define $(package)_build_cmds
  $(MAKE) libdb_cxx-6.2.a libdb-6.2.a all
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  cd $(BASEDIR)/../zcutil && \
  mkdir -p bin && \
  mv -f $($(package)_staging_dir)$(host_prefix)/bin/db_* bin
endef
else
# The BDB utilities silently fail to link on native macOS, causing the rest of
# the install to fail due to missing binaries. Until we can figure out how to
# make them work, avoid building them.
define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install_lib install_include
endef
endif
