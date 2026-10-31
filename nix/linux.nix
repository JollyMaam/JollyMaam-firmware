{ pkgs, hidBackend ? "hidraw", withZlib ? true }:
assert builtins.elem hidBackend [ "libusb" "hidraw" ];
let static = pkgs.pkgsStatic; in
static.stdenv.mkDerivation {
  pname = "jollymaam";
  version = "0.1";
  src = pkgs.lib.cleanSource ../.;
  nativeBuildInputs = [ pkgs.pkg-config pkgs.python3 ];
  buildInputs = [ static.hidapi static.libusb1 static.libudev-zero ] ++ pkgs.lib.optional withZlib static.zlib;
  enableParallelBuilding = true;
  makeFlags = [ "JOLLYMAAM_STATIC=1" "HIDAPI_PKG=hidapi-${hidBackend}" "PREFIX=${placeholder "out"}" ];
  buildFlags = [ "jollymaam" ];
  doCheck = true;
  checkTarget = "libjollymaam-test libjollymaam-linux-test jollymaam-link-check"
    + pkgs.lib.optionalString withZlib " jollymaam-static-deps-check";
  installTargets = [ "install" ];
}
