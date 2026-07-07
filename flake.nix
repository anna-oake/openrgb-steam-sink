{
  description = "OpenRGB Steam Sink plugin devshell";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";
  inputs.systems.url = "github:nix-systems/default";
  inputs.flake-utils = {
    url = "github:numtide/flake-utils";
    inputs.systems.follows = "systems";
  };
  inputs.openrgb-src = {
    url = "gitlab:CalcProgrammer1/OpenRGB?ref=release_candidate_1.0rc2";
    flake = false;
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      openrgb-src,
      ...
    }:
    flake-utils.lib.eachSystem [ "aarch64-darwin" "x86_64-linux" ] (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShells.default = pkgs.mkShell {
          packages =
            with pkgs;
            [
              gnumake
              pkg-config
              qt6.qtbase
            ]
            ++ lib.optionals stdenv.hostPlatform.isLinux [
              openrgb
            ];

          OPENRGB_SOURCE_DIR = openrgb-src;
        };
      }
    );
}
