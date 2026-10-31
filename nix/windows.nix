{ pkgs }:
let
  target = pkgs.pkgsCross.mingwW64;
  hidapi = target.hidapi.overrideAttrs (old: {
    cmakeFlags = (old.cmakeFlags or []) ++ [ "-DBUILD_SHARED_LIBS=OFF" ];
  });
in target.stdenv.mkDerivation {
  pname = "jollymaam-windows";
  version = "0.1";
  src = pkgs.lib.cleanSource ../.;
  nativeBuildInputs = [ pkgs.pkg-config pkgs.python3 ];
  buildInputs = [ hidapi ];
  enableParallelBuilding = true;
  makeFlags = [ "JOLLYMAAM_STATIC=1" "JOLLYMAAM_PLATFORM=windows" "PREFIX=${placeholder "out"}" ];
  buildFlags = [ "jollymaam" "jollymaam-windows-test-build" "jollymaam-link-check" ];
  installTargets = [ "install" "install-jollymaam-tests" ];
}
