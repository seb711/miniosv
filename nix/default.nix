{
  nixpkgs,
  system,
  self,
  apps,
}:

let
  pkgs = nixpkgs.legacyPackages.${system};
  llvmPkgs = pkgs.llvmPackages_20;
  inherit (nixpkgs) lib;

  # Per-arch metadata. Add a new target arch by adding a row here.
  arches = {
    x64 = {
      linuxName = "x86_64";
      qemuName = "x86_64";
      firmwarePrefix = "OVMF";
      crossPkgs = pkgs.pkgsCross.gnu64;
    };
    aarch64 = {
      linuxName = "aarch64";
      qemuName = "aarch64";
      firmwarePrefix = "AAVMF";
      crossPkgs = pkgs.pkgsCross.aarch64-multiplatform;
    };
  };

  hostArch = if system == "x86_64-linux" then "x64" else "aarch64";

  llvmSource = pkgs.callPackage ./llvm-source.nix { };
  toolchain = import ./toolchain.nix { inherit pkgs llvmPkgs system; };

  # Narrowed source for compiler-rt / llvm-libc / libcxx: only the files their
  # scripts actually read. Keeps toolchain drv hashes invariant to unrelated
  # changes in the flake tree.
  toolchainSrc =
    let
      root = toString self;
      keep = [
        "scripts"
        "external/llvm-libc-config"
        "include/api"
      ];
      keepAncestor = [
        "external"
        "include"
      ];
    in
    builtins.path {
      path = self;
      name = "miniosv-toolchain-src";
      filter =
        path: _:
        let
          rel = lib.removePrefix (root + "/") (toString path);
        in
        builtins.any (p: rel == p || lib.hasPrefix (p + "/") rel) keep || builtins.elem rel keepAncestor;
    };

  # Per-arch toolchain sub-builds (independent of the selected app).
  toolchainFor =
    targetArch:
    let
      args = {
        inherit toolchain llvmSource targetArch;
        src = toolchainSrc;
      };
    in
    {
      compilerRt = pkgs.callPackage ./compiler-rt.nix (args // { isCross = targetArch != hostArch; });
      llvmLibc = pkgs.callPackage ./llvm-libc.nix args;
      libcxx = pkgs.callPackage ./libcxx.nix args;
    };

  archToolchains = lib.mapAttrs (a: _: toolchainFor a) arches;

  # (targetArch, app) -> { miniosv, run, awsDeploy } derivations.
  buildVariant =
    {
      targetArch,
      appName,
      appSrc,
    }:
    let
      arch = arches.${targetArch};
      isNative = targetArch == hostArch;
      ovmfPkg = if isNative then pkgs.OVMF else arch.crossPkgs.OVMF;
    in
    rec {
      miniosv = pkgs.callPackage ./miniosv.nix (
        {
          inherit
            toolchain
            llvmSource
            targetArch
            self
            appName
            appSrc
            ;
        }
        // archToolchains.${targetArch}
      );

      run = import ./run.nix {
        inherit
          pkgs
          lib
          miniosv
          self
          ;
        inherit (arch) qemuName firmwarePrefix;
        ovmfFdDir = "${ovmfPkg.fd}/FV";
      };

      awsDeploy = import ./aws-deploy.nix {
        inherit
          pkgs
          lib
          miniosv
          self
          ;
      };
    };

  # Cross-product: apps × arches → { "<app>-<linuxName>" = variant; ... }.
  variants = lib.concatMapAttrs (
    appName: appSrc:
    lib.mapAttrs' (
      targetArch: arch:
      lib.nameValuePair "${appName}-${arch.linuxName}" (buildVariant {
        inherit targetArch appName appSrc;
      })
    ) arches
  ) apps;

  mkApp = binName: pkg: {
    type = "app";
    program = "${pkg}/bin/${binName}";
  };
in
{
  packages = lib.mapAttrs (_: v: v.miniosv) variants;

  apps =
    lib.mapAttrs (_: v: mkApp "miniosv-run" v.run) variants
    // lib.mapAttrs' (
      name: v: lib.nameValuePair "aws-deploy-${name}" (mkApp "miniosv-aws-deploy" v.awsDeploy)
    ) variants;

  devShells = import ./devshells.nix { inherit pkgs toolchain system; };
}
