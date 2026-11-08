{
  description = "lense: show the memory layout of C and C++ records and their vtables";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems
        (system: f nixpkgs.legacyPackages.${system});

      # Clang's C++ API changes every major release; CMake enforces the same pin.
      llvmFor = pkgs: pkgs.llvmPackages_23;

      # Compilers that the fuzzer uses to check lense against real object code.
      # Clang 23 must agree with lense. GCC is a second, independent
      # implementation whose disagreements are reported but do not fail the
      # run. Cross-target static checks use the unwrapped clang, because the
      # Nix wrapper adds host-only flags.
      oracleEnv = pkgs: {
        LENSE_ORACLE_CXX = "${(llvmFor pkgs).clang}/bin/clang++";
        LENSE_ADVISORY_CXX = "${pkgs.gcc}/bin/g++";
        LENSE_STATIC_CXX = "${(llvmFor pkgs).clang-unwrapped}/bin/clang++";
      };
    in
    {
      packages = forAllSystems (pkgs:
        let llvm = llvmFor pkgs; in {
          default = llvm.stdenv.mkDerivation (oracleEnv pkgs // {
            pname = "lense";
            version = "0.1.0";
            src = nixpkgs.lib.fileset.toSource {
              root = ./.;
              fileset = nixpkgs.lib.fileset.unions [
                ./CMakeLists.txt
                ./src
                ./tests
              ];
            };
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
            buildInputs = [ llvm.llvm llvm.libclang ];
            nativeCheckInputs = [ pkgs.python3 ];
            doCheck = true;
            checkPhase = ''
              runHook preCheck
              ctest --output-on-failure -j $NIX_BUILD_CORES
              runHook postCheck
            '';
            meta = {
              description = "Show the memory layout of C and C++ records and their vtables";
              mainProgram = "lense";
              platforms = systems;
            };
          });
        });

      checks = forAllSystems (pkgs: {
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      });

      devShells = forAllSystems (pkgs:
        let llvm = llvmFor pkgs; in {
          default = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) (oracleEnv pkgs // {
            inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
            # GCC stays off PATH (it would shadow cc/c++); tests use the path below.
            packages = [ llvm.clang-tools ];
          });
        });

      formatter = forAllSystems (pkgs: pkgs.nixpkgs-fmt);
    };
}
