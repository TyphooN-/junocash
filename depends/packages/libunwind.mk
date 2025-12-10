package=libunwind
$(package)_version=18.1.6-1

ifneq ($(host_os),mingw32)
# libunwind is provided by libcxx on Linux/native
else
$(package)_download_path=https://repo.msys2.org/mingw/ucrt64
$(package)_download_file=mingw-w64-ucrt-x86_64-libunwind-$($(package)_version)-any.pkg.tar.zst
$(package)_file_name=mingw-w64-ucrt-x86_64-libunwind-$($(package)_version)-any.pkg.tar.zst
$(package)_sha256_hash=f83663c13770d9f2c31d2d79d0d7b88693e0d409e6f03ea1a59634b9263d9d3f

define $(package)_stage_cmds
  echo "Listing extract dir:" && ls -R && \
  mkdir -p $($(package)_staging_prefix_dir)/lib && \
  cp lib/libunwind.a $($(package)_staging_prefix_dir)/lib && \
  cp lib/libunwind.dll.a $($(package)_staging_prefix_dir)/lib
endef
endif

