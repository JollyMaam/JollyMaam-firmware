{ pkgs ? import <nixpkgs> {}, hidBackend ? "libusb", withZlib ? true }:
assert builtins.elem hidBackend [ "libusb" "hidraw" ];
let static = pkgs.pkgsStatic; in
static.stdenv.mkDerivation {
  pname = "jollymaam";
  version = "0.1";
  src = pkgs.lib.cleanSource ./.;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ static.hidapi static.libusb1 static.libudev-zero ] ++ pkgs.lib.optional withZlib static.zlib;
  buildPhase = ''
    runHook preBuild
    mkdir -p build/static
    for source in src/lib/jieli_image/*.c src/lib/proto_jieli.c src/lib/proto_8bitdo.c src/lib/proto_jollymaam.c src/lib/upgrade_8bitdo.c; do
      $CC -std=c99 -O2 -Wall -Wextra -c "$source" -o "build/static/$(basename "$source" .c).o"
    done
    $AR rcs build/static/libjollymaam-core.a build/static/*.o
    $CC -std=c99 -O2 -Wall -Wextra -c src/lib/transport_hid.c \
      $(pkg-config --cflags hidapi-${hidBackend}) -o build/static/transport_hid.o
    $CC -std=c99 -O2 -Wall -Wextra -c src/lib/transport_linux.c \
      -o build/static/transport_linux.o
    # MRI ADDLIB copies archive members, rather than nesting .a files.
    # Select exactly one hidapi backend: both export the same public symbols.
    cat > build/static/merge.mri <<MERGE
    CREATE build/static/libjollymaam.a
    ADDLIB build/static/libjollymaam-core.a
    ADDMOD build/static/transport_hid.o
    ADDMOD build/static/transport_linux.o
    ADDLIB $(pkg-config --variable=libdir hidapi-${hidBackend})/libhidapi-${hidBackend}.a
    ADDLIB $(pkg-config --variable=libdir libusb-1.0)/libusb-1.0.a
    ADDLIB $(pkg-config --variable=libdir libudev)/libudev.a
    ${pkgs.lib.optionalString withZlib "ADDLIB $(pkg-config --variable=libdir zlib)/libz.a"}
    SAVE
    END
    MERGE
    $AR -M < build/static/merge.mri
    $RANLIB build/static/libjollymaam.a
    # Consumers need only our archive and the target C runtime.
    $CC -std=c99 -O2 -Wall -Wextra tools/jollymaam.c \
      build/static/libjollymaam.a -static -pthread -o build/static/jollymaam
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
    ${pkgs.lib.optionalString withZlib ''
    # These APIs must resolve from the bundled archive, without -lusb/-lhidapi/-lz.
    $CC -std=c99 tools/test_static_dependencies.c \
      $(pkg-config --cflags hidapi-${hidBackend} libusb-1.0 zlib) \
      build/static/libjollymaam.a -static -pthread -o build/static/test-bundled-deps
    build/static/test-bundled-deps
    $READELF -d build/static/test-bundled-deps > build/static/dependency-tags
    if grep -q NEEDED build/static/dependency-tags; then exit 1; fi
    ''}
    $READELF -l build/static/jollymaam > build/static/program-headers
    $READELF -d build/static/jollymaam > build/static/dynamic-tags
    if grep -q INTERP build/static/program-headers; then exit 1; fi
    if grep -q NEEDED build/static/dynamic-tags; then exit 1; fi
  '';
  installPhase = ''
    mkdir -p $out/bin $out/lib $out/include/jollymaam/jieli_image
    cp build/static/jollymaam $out/bin/
    cp build/static/libjollymaam.a build/static/libjollymaam-core.a $out/lib/
    cp src/lib/*.h $out/include/jollymaam/
    cp src/lib/jieli_image/*.h $out/include/jollymaam/jieli_image/
  '';
}
