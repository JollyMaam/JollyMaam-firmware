{ pkgs ? import <nixpkgs> {} }:
let static = pkgs.pkgsStatic; in
static.stdenv.mkDerivation {
  pname = "jollymaam";
  version = "0.1";
  src = pkgs.lib.cleanSource ./.;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ static.hidapi static.libudev-zero ];
  buildPhase = ''
    runHook preBuild
    mkdir -p build/static
    for source in src/lib/jieli_image/*.c src/lib/proto_jieli.c src/lib/proto_8bitdo.c src/lib/proto_jollymaam.c src/lib/upgrade_8bitdo.c; do
      $CC -std=c99 -O2 -Wall -Wextra -c "$source" -o "build/static/$(basename "$source" .c).o"
    done
    $AR rcs build/static/libjollymaam.a build/static/*.o
    $CC -std=c99 -O2 -Wall -Wextra tools/jollymaam.c src/lib/transport_hid.c \
      build/static/libjollymaam.a $(pkg-config --static --cflags --libs hidapi-hidraw libudev) \
      -static -o build/static/jollymaam
    runHook postBuild
  '';
  doCheck = true;
  checkPhase = ''
    for name in bankcb crypto entry key ufw; do
      $CC -std=c99 -O2 -DUNITTEST src/lib/jieli_image/$name.c \
        build/static/libjollymaam.a -static -o build/static/test-$name
      build/static/test-$name
    done
    for name in proto_8bitdo proto_jieli proto_jollymaam; do
      $CC -std=c99 -O2 -DUNITTEST src/lib/$name.c \
        build/static/libjollymaam.a -static -o build/static/test-$name
      build/static/test-$name
    done
    $CC -std=c99 -O2 tools/test_jollymaam_upgrade.c build/static/libjollymaam.a \
      -static -o build/static/test-upgrade
    build/static/test-upgrade
    $READELF -l build/static/jollymaam > build/static/program-headers
    $READELF -d build/static/jollymaam > build/static/dynamic-tags
    if grep -q INTERP build/static/program-headers; then exit 1; fi
    if grep -q NEEDED build/static/dynamic-tags; then exit 1; fi
  '';
  installPhase = ''
    mkdir -p $out/bin $out/lib $out/include/jollymaam/jieli_image
    cp build/static/jollymaam $out/bin/
    cp build/static/libjollymaam.a $out/lib/
    cp src/lib/*.h $out/include/jollymaam/
    cp src/lib/jieli_image/*.h $out/include/jollymaam/jieli_image/
  '';
}
