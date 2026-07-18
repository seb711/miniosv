{ nixpkgs, system, self }:

# Per-system entry point: turns the flake inputs into the packages / apps /
# devShells outputs. flake.nix just calls into here via flake-utils.
#
# Both host arches (x86_64-linux, aarch64-linux) can build either target arch
# (x64, aarch64) because the pure-LLVM toolchain cross-compiles by
# --target=<triple>. So for each host we expose:
#   packages.miniosv           - image for the host arch (default)
#   packages.miniosv-x86_64    - image for x86_64
#   packages.miniosv-aarch64   - image for aarch64
#   apps.default               - run the host-arch image under qemu
#   apps.run-x86_64            - run the x86_64 image under qemu
#   apps.run-aarch64           - run the aarch64 image under qemu

let
  pkgs = nixpkgs.legacyPackages.${system};
  llvmPkgs = pkgs.llvmPackages_20;

  hostArch = if system == "x86_64-linux" then "x64" else "aarch64";

  llvmSource = pkgs.callPackage ./llvm-source.nix { };
  toolchain = import ./toolchain.nix { inherit pkgs llvmPkgs system; };

  # One factory per target arch, so cross- and native builds are structured
  # identically.
  forArch = targetArch:
    let
      commonArgs = { inherit toolchain llvmSource targetArch self system; };
      compilerRt = pkgs.callPackage ./compiler-rt.nix commonArgs;
      llvmLibc   = pkgs.callPackage ./llvm-libc.nix   commonArgs;
      libcxx     = pkgs.callPackage ./libcxx.nix      commonArgs;
      miniosv    = pkgs.callPackage ./miniosv.nix (commonArgs // {
        inherit compilerRt llvmLibc libcxx;
      });
      run = import ./run.nix {
        inherit pkgs lib miniosv targetArch self system;
      };
      awsDeploy = import ./aws-deploy.nix {
        inherit pkgs lib miniosv targetArch self;
      };
    in { inherit compilerRt llvmLibc libcxx miniosv run awsDeploy; };

  inherit (nixpkgs) lib;
  x64Build = forArch "x64";
  aarch64Build = forArch "aarch64";
  hostBuild = if hostArch == "x64" then x64Build else aarch64Build;

  mkApp = binName: pkg: {
    type = "app";
    program = "${pkg}/bin/${binName}";
  };
in
{
  packages = {
    default = hostBuild.miniosv;
    miniosv = hostBuild.miniosv;
    miniosv-x86_64 = x64Build.miniosv;
    miniosv-aarch64 = aarch64Build.miniosv;

    # Toolchain sub-builds exposed for debugging / caching.
    compiler-rt-x86_64 = x64Build.compilerRt;
    compiler-rt-aarch64 = aarch64Build.compilerRt;
    llvm-libc-x86_64 = x64Build.llvmLibc;
    llvm-libc-aarch64 = aarch64Build.llvmLibc;
    libcxx-x86_64 = x64Build.libcxx;
    libcxx-aarch64 = aarch64Build.libcxx;
  };

  apps = {
    default = mkApp "miniosv-run" hostBuild.run;
    run-x86_64 = mkApp "miniosv-run" x64Build.run;
    run-aarch64 = mkApp "miniosv-run" aarch64Build.run;
    aws-deploy-x86_64  = mkApp "miniosv-aws-deploy" x64Build.awsDeploy;
    aws-deploy-aarch64 = mkApp "miniosv-aws-deploy" aarch64Build.awsDeploy;
  };

  devShells = import ./devshells.nix { inherit pkgs toolchain system; };
}
