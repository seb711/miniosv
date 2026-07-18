{ pkgs, lib, miniosv, targetArch, self }:

# Wrap scripts/aws-deploy.py with its Python + AWS deps pinned from nixpkgs
# and the boot image path pinned to the built miniosv derivation.  Positional
# args pass through, so a typical invocation is:
#   nix run .#aws-deploy-x86_64  -- eu-north-1 c5.large --attach
#   nix run .#aws-deploy-aarch64 -- eu-north-1 c7g.large --attach
#
# Boots the image on the matching Nitro instance family (c5=x86_64, c7g=arm64)
# and (with --attach) streams the serial console; Ctrl+C tears down instance,
# AMI, snapshot.

let
  pythonEnv = pkgs.python3.withPackages (ps: with ps; [
    awscrt
    boto3
    botocore
    pyyaml
  ]);
in
pkgs.writeShellScriptBin "miniosv-aws-deploy" ''
  export PATH=${lib.makeBinPath [ pythonEnv pkgs.awscli2 pkgs.qemu pkgs.coreutils ]}:$PATH
  exec ${pythonEnv}/bin/python3 ${self}/scripts/aws-deploy.py \
      --image ${miniosv}/share/miniosv/loader.img \
      "$@"
''
