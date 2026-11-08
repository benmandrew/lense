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

      # Emscripten writes its cache of compiled system libraries as it goes.
      emscriptenSetup = pkgs: ''
        export HOME=$TMPDIR EM_CACHE=$TMPDIR/emscripten-cache
        cp -r ${pkgs.emscripten}/share/emscripten/cache $EM_CACHE
        chmod -R u+w $EM_CACHE
      '';
    in
    {
      packages = forAllSystems (pkgs:
        let llvm = llvmFor pkgs; in rec {
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

          # The clang libraries that lense needs, compiled to WebAssembly. The
          # build tree is the output, because LLVM's build-tree CMake package
          # refers to it by absolute path; object files are deleted.
          llvm-wasm = pkgs.stdenv.mkDerivation {
            pname = "lense-llvm-wasm";
            inherit (llvm.llvm) version;
            src = llvm.llvm.monorepoSrc;
            # Configure against the store path: the build-tree CMake package
            # refers to the source directory, so it must outlive the build.
            dontUnpack = true;
            nativeBuildInputs = [ pkgs.emscripten pkgs.cmake pkgs.ninja pkgs.python3 ];
            configurePhase = ''
              runHook preConfigure
              ${emscriptenSetup pkgs}
              emcmake cmake -G Ninja -S $src/llvm -B $out/build -C ${./wasm/llvm-cache.cmake} \
                -DLLVM_NATIVE_TOOL_DIR=${llvm.tblgen}/bin \
                -DLLVM_TABLEGEN=${llvm.tblgen}/bin/llvm-tblgen \
                -DCLANG_TABLEGEN=${llvm.tblgen}/bin/clang-tblgen
              runHook postConfigure
            '';
            buildPhase = ''
              runHook preBuild
              ninja -C $out/build -j $NIX_BUILD_CORES clangTooling clang-resource-headers
              find $out/build -name '*.o' -delete
              runHook postBuild
            '';
            installPhase = "true";
            dontFixup = true;
          };

          # lense compiled to WebAssembly: share/lense/web holds the browser
          # module. The test suite runs the Node build of the CLI.
          wasm = pkgs.stdenv.mkDerivation (oracleEnv pkgs // {
            pname = "lense-wasm";
            version = "0.1.0";
            src = nixpkgs.lib.fileset.toSource {
              root = ./.;
              fileset = nixpkgs.lib.fileset.unions [
                ./CMakeLists.txt
                ./src
                ./tests
                ./wasm
              ];
            };
            nativeBuildInputs = [ pkgs.emscripten pkgs.cmake pkgs.ninja pkgs.python3 pkgs.nodejs ];
            configurePhase = ''
              runHook preConfigure
              ${emscriptenSetup pkgs}
              emcmake cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel \
                -DLLVM_DIR=${llvm-wasm}/build/lib/cmake/llvm -DCMAKE_INSTALL_PREFIX=$out
              runHook postConfigure
            '';
            buildPhase = "ninja -C build -j $NIX_BUILD_CORES";
            doCheck = true;
            checkPhase = "ctest --test-dir build --output-on-failure -j $NIX_BUILD_CORES";
            installPhase = "ninja -C build install";
            dontFixup = true;
          });

          # The browser front end, ready to serve as static files.
          web = pkgs.buildNpmPackage {
            pname = "lense-web";
            version = "0.1.0";
            src = ./web;
            npmDeps = pkgs.importNpmLock { npmRoot = ./web; };
            npmConfigHook = pkgs.importNpmLock.npmConfigHook;
            LENSE_WASM_DIR = "${wasm}/share/lense/web";
            LENSE_EXAMPLES_DIR = ./tests/cases;
            installPhase = "cp -r dist $out";
          };
        });

      checks = forAllSystems (pkgs: {
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      });

      devShells = forAllSystems (pkgs:
        let llvm = llvmFor pkgs; in {
          default = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) (oracleEnv pkgs // {
            inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
            # GCC stays off PATH (it would shadow cc/c++); tests use the path below.
            packages = [ llvm.clang-tools pkgs.emscripten pkgs.nodejs ];
          });
        });

      formatter = forAllSystems (pkgs: pkgs.nixpkgs-fmt);
    };
}
