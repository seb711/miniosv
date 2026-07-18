{ pkgs, lib, miniosv, targetArch, self, system }:

# A small wrapper around scripts/run.py that pins the boot image (from the
# built miniosv derivation) and the UEFI firmware paths (from nixpkgs). Bare
# `nix run` invokes this, and any extra args pass through to run.py:
#   nix run . -- --debug --wait
#
# targetArch is the OSv arch ("x64" or "aarch64"). The qemu emulator name uses
# the Linux triple form: x86_64 / aarch64.
#
# Firmware handling:
#   * host arch == target arch: bind {OVMF,AAVMF}_{CODE,VARS} to nixpkgs' fd.
#   * host arch != target arch: leave the env vars unset — cross-arch firmware
#     is not on cache.nixos.org (edk2/openssl compile takes 15+ min via
#     pkgsCross), and run.py already falls back to distro paths (/usr/share/
#     AAVMF etc.) and env-var overrides. Users cross-running should either
#     pre-fetch cross firmware or set {OVMF,AAVMF}_{CODE,VARS} themselves.

let
  qemuArch = if targetArch == "x64" then "x86_64" else "aarch64";
  hostArch = if system == "x86_64-linux" then "x64" else "aarch64";
  isNative = targetArch == hostArch;

  firmwarePrefix = if targetArch == "aarch64" then "AAVMF" else "OVMF";
  firmwareExports = lib.optionalString isNative ''
    export ${firmwarePrefix}_CODE="${pkgs.OVMF.fd}/FV/${firmwarePrefix}_CODE.fd"
    export ${firmwarePrefix}_VARS="${pkgs.OVMF.fd}/FV/${firmwarePrefix}_VARS.fd"
  '';
in
pkgs.writeShellScriptBin "miniosv-run" ''
  export PATH=${lib.makeBinPath [ pkgs.qemu pkgs.python3 pkgs.coreutils ]}:$PATH
  ${firmwareExports}
  exec ${self}/scripts/run.py \
      --arch ${qemuArch} \
      --image-path ${miniosv}/share/miniosv/loader.img \
      "$@"
''
