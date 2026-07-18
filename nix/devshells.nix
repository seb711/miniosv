{ pkgs, toolchain, system }:

# Same three developer shells the old flake shipped, moved verbatim so
# `nix develop`, `nix develop .#aws`, `nix develop .#cli` keep working. Nothing
# here is used by the packages / apps outputs.

let
  ovmf_prefix = if system == "x86_64-linux" then "OVMF" else "AAVMF";
in
rec {
  default = pkgs.mkShell {
    nativeBuildInputs = toolchain.buildInputs ++ [
      pkgs.qemu
      pkgs.gdb
    ];

    # UEFI boot requires OVMF installation
    "${ovmf_prefix}_CODE" = "${pkgs.OVMF.fd}/FV/${ovmf_prefix}_CODE.fd";
    "${ovmf_prefix}_VARS" = "${pkgs.OVMF.fd}/FV/${ovmf_prefix}_VARS.fd";
  };

  aws = default.overrideAttrs (default: {
    nativeBuildInputs = [
      pkgs.awscli2
      (pkgs.python3.withPackages (
        ps: with ps; [
          awscrt
          boto3
          botocore
          # We need to redeclare every python
          # dependency from the default shell
          pyyaml
        ]
      ))
    ]
    ++ default.nativeBuildInputs;
  });

  cli = aws.overrideAttrs (default: {
    nativeBuildInputs =
      with pkgs;
      [
        bear
        black
        clang-tools
        pyright
      ]
      ++ aws.nativeBuildInputs;
  });
}
