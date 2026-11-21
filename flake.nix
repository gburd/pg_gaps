{
  description = "pg_gaps: BayesStore-style probabilistic graphical models for PostgreSQL";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        lib = pkgs.lib;

        # Supported PostgreSQL majors.
        pgVersions = {
          pg17 = pkgs.postgresql_17;
          pg18 = pkgs.postgresql_18;
          pg19 = pg19;
        };

        # PostgreSQL 19.0 is released but this nixpkgs still packages 19rc2.
        # Build 19.0 from the release tag with nixpkgs' own recipe; once
        # nixpkgs carries a final 19.x, its package is used unchanged.
        pg19 =
          if lib.hasInfix "rc" pkgs.postgresql_19.version
            || lib.hasInfix "beta" pkgs.postgresql_19.version
          then pkgs.postgresql_19.overrideAttrs (o: {
            version = "19.0";
            src = pkgs.fetchFromGitHub {
              owner = "postgres";
              repo = "postgres";
              rev = "refs/tags/REL_19_0";
              hash = "sha256-nto9w6pCC15nFiVtH7ZeZS00RxNNy85ZzjZVhqgq3Gc=";
            };
          })
          else pkgs.postgresql_19;

        # The version lives in the control file and nowhere else.
        version = builtins.head (builtins.match
          ".*default_version = '([^']+)'.*"
          (builtins.readFile ./pg_gaps.control));

        # Only tracked-style source goes into the build: no build output, no
        # agent notes, no papers.
        src = lib.fileset.toSource {
          root = ./.;
          fileset = lib.fileset.unions [
            ./Makefile
            ./pg_gaps.control
            ./META.json
            (lib.fileset.fileFilter (f: lib.hasSuffix ".sql" f.name) ./.)
            ./src
            ./expected
            ./test
          ];
        };

        # nixpkgs' PGXS builder installs into $out with DESTDIR and moves the
        # files out of the nested store path, so no install list is needed.
        # It is only reachable through the extension scope's callPackage.
        buildFor = pg: pg.pkgs.callPackage
          ({ postgresqlBuildExtension }: postgresqlBuildExtension {
            pname = "pg_gaps";
            inherit version src;
            enableUpdateScript = false;
            meta = {
              description = "Probabilistic graphical models over incomplete relations";
              license = lib.licenses.postgresql;
              platforms = pg.meta.platforms;
            };
          })
          { };

        # Run REGRESS against a throwaway server that has pg_gaps installed.
        # pg_regress comes from the plain server's PGXS; the server it talks
        # to is the withPackages wrapper, which can load the extension.
        installcheckFor = pg:
          let pgWith = pg.withPackages (_: [ (buildFor pg) ]);
          in pkgs.stdenv.mkDerivation {
            name = "pg_gaps-installcheck-${pg.version}";
            inherit src;
            nativeBuildInputs = [ pgWith pg.pg_config ];
            dontConfigure = true;
            dontInstall = true;
            buildPhase = ''
              runHook preBuild
              export PGDATA=$TMPDIR/pgdata PGHOST=$TMPDIR PGPORT=5432 PGUSER=postgres
              initdb -U postgres --no-locale --encoding=UTF8 >/dev/null
              pg_ctl -D "$PGDATA" -l "$TMPDIR/server.log" -w \
                -o "-k $TMPDIR -c listen_addresses=" start
              make installcheck PG_CONFIG=${pg.pg_config}/bin/pg_config \
                || { cat regression.diffs 2>/dev/null; cat "$TMPDIR/server.log"; exit 1; }
              PGDATABASE=postgres bash test/dump/run.sh \
                || { cat "$TMPDIR/server.log"; exit 1; }
              pg_ctl -D "$PGDATA" -w stop
              touch $out
              runHook postBuild
            '';
          };

        # pg_gaps against pgmpy, an independent implementation (ROADMAP Q5).
        # Kept out of `make installcheck` so distribution builds need no
        # Python.
        pgmpyPython = pkgs.python3.withPackages (ps: [ ps.pgmpy ps.psycopg ]);
        pgmpyFor = pg:
          let pgWith = pg.withPackages (_: [ (buildFor pg) ]);
          in pkgs.stdenv.mkDerivation {
            name = "pg_gaps-pgmpy-${pg.version}";
            inherit src;
            nativeBuildInputs = [ pgWith pgmpyPython ];
            dontConfigure = true;
            dontInstall = true;
            buildPhase = ''
              runHook preBuild
              export PGDATA=$TMPDIR/pgdata PGHOST=$TMPDIR PGPORT=5432 PGUSER=postgres PGDATABASE=postgres
              initdb -U postgres --no-locale --encoding=UTF8 >/dev/null
              pg_ctl -D "$PGDATA" -l "$TMPDIR/server.log" -w \
                -o "-k $TMPDIR -c listen_addresses=" start
              python3 test/pgmpy/check.py --models 300 \
                || { cat "$TMPDIR/server.log"; exit 1; }
              python3 test/pgmpy/em.py --models 200 \
                || { cat "$TMPDIR/server.log"; exit 1; }
              pg_ctl -D "$PGDATA" -w stop
              touch $out
              runHook postBuild
            '';
          };

        packages = builtins.mapAttrs (_: buildFor) pgVersions;

        devPg = pgVersions.pg18;
      in
      {
        packages = packages // { default = packages.pg18; };

        checks = packages
          // lib.mapAttrs' (n: pg: lib.nameValuePair "installcheck-${n}" (installcheckFor pg)) pgVersions
          // {
            pgmpy = pgmpyFor devPg;

            # The standalone engine against brute force, plain, with ASan and
            # UBSan, and with every cache key forced to collide.
            engine = pkgs.runCommand "pg_gaps-engine"
              { nativeBuildInputs = [ pkgs.clang ]; } ''
              CC=clang bash ${src}/test/engine/run.sh 1 20000
              touch $out
            '';

            # The Makefile target is the one implementation of this rule.
            ascii = pkgs.runCommand "pg_gaps-check-ascii"
              { nativeBuildInputs = [ pkgs.gnumake devPg.pg_config ]; } ''
              make -C ${src} check-ascii check-meta
              touch $out
            '';
          };

        devShells.default = pkgs.mkShell {
          name = "pg_gaps-dev";
          packages = [
            devPg
            devPg.pg_config
            # nixpkgs builds PostgreSQL with clang, so PGXS defaults to CC=clang.
            pkgs.clang
            pkgs.gnumake
            pkgs.clang-tools # clangd, for editing
            pkgs.shellcheck
            pkgs.shfmt
          ];
          shellHook = ''
            echo "pg_gaps dev shell: PostgreSQL ${devPg.version}"
            echo "  dev/regress.sh      build out of tree + run the regression suite"
            echo "  nix flake check     the gate: PG 17, 18, 19"
          '';
        };

        formatter = pkgs.nixpkgs-fmt;
      });
}
