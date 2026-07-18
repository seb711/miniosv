{ stdenv, runCommand, toolchain, llvmSource, targetArch, self, system }:

# Provide libclang_rt.builtins.a for the target arch at a stable path.
#
# * Native (target == host): the clang wrapper's resource-dir already links in
#   the right builtins (via nixpkgs' compiler-rt), so we just repackage it.
# * Cross: run scripts/build-compiler-rt.sh, which cross-builds from the pinned
#   llvm-project.  The kernel Makefile keys off the same host/target diff.

let
  hostArch = if system == "x86_64-linux" then "x64" else "aarch64";
  isCross = targetArch != hostArch;
  llvmTriple = if targetArch == "x64" then "x86_64-unknown-linux-gnu"
                                       else "aarch64-unknown-linux-gnu";
in
if !isCross then
  runCommand "miniosv-compiler-rt-22.1.7-${targetArch}" { } ''
    mkdir -p $out/lib
    cp ${toolchain.resourceDir}/lib/${llvmTriple}/libclang_rt.builtins.a $out/lib/
  ''
else
  stdenv.mkDerivation {
    pname = "miniosv-compiler-rt";
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
      bash scripts/build-compiler-rt.sh ${targetArch}
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p $out/lib
      cp build/compiler-rt/${targetArch}/lib/libclang_rt.builtins.a $out/lib/
      runHook postInstall
    '';
  }
