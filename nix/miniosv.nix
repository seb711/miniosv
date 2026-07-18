{ stdenv, lib, toolchain, llvmSource, compilerRt, llvmLibc, libcxx
, targetArch, self, system ? null }:

# Build the miniosv unikernel image (loader.img) for the given target arch.
# Cross-compilation is driven by `arch=<targetArch>` on the make command line;
# the Makefile derives CROSS_PREFIX (and clang's --target=<triple>) from the
# host_arch/target_arch mismatch — pure-LLVM, no GNU cross toolchain.
#
# The three C/C++ runtime archives (compiler-rt, llvm-libc, libcxx) come from
# their own derivations and are pre-staged at the paths make expects, with
# matching stamp files so the in-tree build scripts stay unused.

let
  # nixpkgs stdenv defaults CC/CXX/LD to its wrappers; we drive clang ourselves
  # (with our resource-dir wrapper) via the toolchain input, so unset them.
  archName = targetArch;
in
stdenv.mkDerivation {
  pname = "miniosv";
  version = archName;

  src = self;

  nativeBuildInputs = toolchain.buildInputs;

  dontConfigure = true;
  dontFixup = true;
  enableParallelBuilding = true;

  # nixpkgs' stdenv exports CC=gcc / LD=ld wrappers that would poison the
  # kernel's pure-LLVM link. The Makefile pins CC/CXX/LD itself; strip the
  # env vars so nothing leaks in through the shell.
  CC = "";
  CXX = "";
  LD = "";
  AR = "";
  AS = "";
  NM = "";
  RANLIB = "";
  STRIP = "";
  OBJCOPY = "";
  READELF = "";

  buildPhase = ''
    runHook preBuild

    # ---- Stage the pinned llvm-project source (kernel -isystem needs
    #      libunwind/include, and the Makefile's llvm_project_stamp guards
    #      the whole toolchain sub-build).
    mkdir -p external
    cp -r ${llvmSource} external/llvm-project
    chmod -R u+w external/llvm-project
    touch external/llvm-project/.sparse-ready

    # ---- Stage the three pre-built runtime archives at the paths make
    #      expects, plus their stamp files under $(out).
    mkdir -p build/compiler-rt/${archName}/lib
    cp ${compilerRt}/lib/libclang_rt.builtins.a \
       build/compiler-rt/${archName}/lib/

    mkdir -p build/llvm-libc/${archName}/libc/lib
    cp ${llvmLibc}/lib/libc.a ${llvmLibc}/lib/libm.a \
       build/llvm-libc/${archName}/libc/lib/

    mkdir -p build/libcxx/${archName}/lib build/libcxx/${archName}/include
    cp ${libcxx}/lib/libc++.a    build/libcxx/${archName}/lib/
    cp ${libcxx}/lib/libc++abi.a build/libcxx/${archName}/lib/
    cp ${libcxx}/lib/libunwind.a build/libcxx/${archName}/lib/
    cp -r ${libcxx}/include/c++ build/libcxx/${archName}/include/

    outDir=build/release.${archName}
    mkdir -p "$outDir"
    # Stamps first, archives already exist and are older — touch archives
    # again so they win the mtime race and no rule fires.
    touch "$outDir/.compiler-rt-built"
    touch "$outDir/.llvm-libc-built"
    touch "$outDir/.libcxx-built"
    touch build/compiler-rt/${archName}/lib/libclang_rt.builtins.a
    touch build/llvm-libc/${archName}/libc/lib/*.a
    touch build/libcxx/${archName}/lib/*.a

    patchShebangs scripts

    # Nail down the strip / objcopy / readelf the Makefile picks. The lookup
    # is `which llvm-strip || which llvm-strip-20 || echo strip`; the fallback
    # (GNU binutils) works for a native x86_64 build but can't recognise
    # aarch64 ELF on an x86_64 host. Force llvm-* directly.
    make -j"$NIX_BUILD_CORES" arch=${archName} \
        STRIP=llvm-strip OBJCOPY=llvm-objcopy READELF=llvm-readelf

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/share/miniosv $out/bin
    cp build/release.${archName}/loader.img         $out/share/miniosv/
    cp build/release.${archName}/loader.efi         $out/share/miniosv/
    cp build/release.${archName}/loader.elf         $out/share/miniosv/
    cp build/release.${archName}/loader-stripped.elf $out/share/miniosv/
    runHook postInstall
  '';

  meta = with lib; {
    description = "miniosv unikernel image (${archName})";
    platforms = [ "x86_64-linux" "aarch64-linux" ];
  };
}
