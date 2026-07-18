{
  description = "miniosv — slim unikernel OS";

  # Each app is staged into app/ inside the build sandbox and compiled into
  # the kernel image. To add an app: (1) add an input pointing at its source
  # (any tree with a Makefile fragment declaring $(app-objects) and an
  # osv_app_main entry point), then (2) add it to `apps` below. Every app
  # gets image, run, and aws-deploy outputs for both x86_64 and aarch64.
  #
  # Flake outputs (per system):
  #   packages.<app>-<arch>              - loader.img
  #   apps.<app>-<arch>                  - QEMU boot wrapper
  #   apps.aws-deploy-<app>-<arch>       - AWS deploy wrapper
  #   devShells.{default,aws,cli}        - developer environments

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";

    miniduckdb = {
      url = "github:Martin-Lndbl/miniduckdb/miniosv-trunk";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      miniduckdb,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      import ./nix {
        inherit nixpkgs system self;
        apps = {
          native-example = ./native-example;
          inherit miniduckdb;
        };
      }
    );
}
