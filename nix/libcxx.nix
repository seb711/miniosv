{ stdenv, toolchain, llvmSource, targetArch, self, system ? null }:

# Build libc++ / libc++abi / libunwind as static archives for the target arch,
# via scripts/build-libcxx.sh. We pre-stage the llvm-project sparse checkout so
# the script skips its `git clone`; the script still needs to sed a couple of
# libc++abi / libunwind sources, hence the writable copy.
#
# The kernel Makefile expects the archives *and* the libc++ v1 headers at a
# stable path inside its build tree (build/libcxx/<arch>/{lib,include}), so we
# publish that whole subtree here — with -L to dereference the cmake-generated
# header symlinks back to their upstream sources, so the derivation is
# self-contained.

stdenv.mkDerivation {
  pname = "miniosv-libcxx";
  version = "22.1.7-${targetArch}";

  src = self;

  nativeBuildInputs = toolchain.buildInputs;

  dontConfigure = true;
  dontFixup = true;
  enableParallelBuilding = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p external
    cp -r ${llvmSource} external/llvm-project
    chmod -R u+w external/llvm-project

    patchShebangs scripts
    bash scripts/build-libcxx.sh ${targetArch}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib $out/include
    cp    build/libcxx/${targetArch}/lib/libc++.a    $out/lib/
    cp    build/libcxx/${targetArch}/lib/libc++abi.a $out/lib/
    cp    build/libcxx/${targetArch}/lib/libunwind.a $out/lib/
    cp -rL build/libcxx/${targetArch}/include/c++    $out/include/
    runHook postInstall
  '';
}
