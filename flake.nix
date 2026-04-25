{
  description = "QEMU Camp 2026 development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forEachSystem = f:
        nixpkgs.lib.genAttrs systems (system:
          f (import nixpkgs { inherit system; })
        );
    in
    {
      devShells = forEachSystem (pkgs:
        let
          crossCc = pkgs.pkgsCross.riscv64-embedded.stdenv.cc;
        in
        {
          default = pkgs.mkShell {
            name = "qemu-camp-devshell";

            # Reuse the upstream qemu package dependency closure,
            # then extend it with camp-specific toolchains.
            inputsFrom = [ pkgs.qemu ];
            packages = with pkgs; [
              crossCc
              rustc
              cargo
              rustfmt
              rust-bindgen
              clang
              llvmPackages.libclang
              git
              gdb
            ];

            # Match toolchain prefix expected by Makefile.camp via env override.
            CROSS_PREFIX = crossCc.targetPrefix;
            LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
            hardeningDisable = [ "fortify" ];

            shellHook = ''
              export PATH="${crossCc}/bin:$PATH"
              exec fish
            '';
          };
        });
    };
}
