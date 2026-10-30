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
  buildPhase = ''
    mkdir -p build/windows
    for source in src/lib/jieli_image/*.c src/lib/proto_*.c src/lib/upgrade_*.c src/lib/flash_jieli.c src/lib/package.c src/lib/sha256.c src/lib/manifest.c src/lib/session.c; do
      $CC -std=c99 -O2 -Wall -Wextra -Werror -c "$source" -o "build/windows/$(basename "$source" .c).o"
    done
    $AR rcs build/windows/libjollymaam.a build/windows/*.o
    mkdir -p build/windows/platform
    for source in src/lib/platform.c src/lib/transport_hid.c src/lib/transport_custom.c src/lib/transport_jieli.c src/lib/transport_win32.c; do
      $CC -std=c99 -O2 -Wall -Wextra -Werror $(pkg-config --cflags hidapi) -c "$source" \
        -o "build/windows/platform/$(basename "$source" .c).o"
    done
    $AR rcs build/windows/libjollymaam-platform.a build/windows/platform/*.o
    $CC -std=c99 -O2 -Wall -Wextra -Werror tools/jollymaam.c tools/jollymaam_update.c \
      build/windows/libjollymaam-platform.a build/windows/libjollymaam.a \
      $(pkg-config --static --cflags --libs hidapi) -static -static-libgcc -lsetupapi -lhid -o build/windows/jollymaam.exe
    mkdir -p build/windows/tests
    for name in builder bankcb crypto entry image key ufw; do
      $CC -std=c99 -O2 -Wall -Wextra -Werror -DUNITTEST src/lib/jieli_image/$name.c \
        build/windows/libjollymaam.a -static -o build/windows/tests/$name.exe
    done
    for name in proto_8bitdo proto_jieli proto_jollymaam upgrade_jollymaam upgrade_jieli sha256; do
      $CC -std=c99 -O2 -Wall -Wextra -Werror -DUNITTEST src/lib/$name.c \
        build/windows/libjollymaam.a -static -o build/windows/tests/$name.exe
    done
    for name in upgrade jieli session; do
      $CC -std=c99 -O2 -Wall -Wextra -Werror tools/test_jollymaam_$name.c \
        build/windows/libjollymaam.a -static -o build/windows/tests/simulate-$name.exe
    done
    $CC -std=c99 -O2 -Wall -Wextra -Werror tools/test_jollymaam_win32.c \
      -static -lsetupapi -o build/windows/tests/scsi-win32.exe
    python3 -c 'from tools.test_jollymaam_app import STUBS; print(STUBS)' > build/windows/app-stubs.c
    $CC -std=c99 -O2 -Wall -Wextra -Werror -I. build/windows/app-stubs.c tools/jollymaam_update.c \
      build/windows/libjollymaam.a -static -o build/windows/tests/app-mock.exe
    $OBJDUMP -p build/windows/jollymaam.exe > build/windows/imports.txt
    for dll in $(awk '/DLL Name:/ {print tolower($3)}' build/windows/imports.txt); do
      case "$dll" in hid.dll|kernel32.dll|msvcrt.dll|setupapi.dll) ;;
        *) echo "Unexpected runtime DLL: $dll"; exit 1;;
      esac
    done
  '';
  installPhase = ''
    mkdir -p $out/bin $out/lib $out/include/jollymaam/jieli_image $out/share/jollymaam
    cp build/windows/jollymaam.exe $out/bin/
    cp build/windows/libjollymaam.a build/windows/libjollymaam-platform.a $out/lib/
    cp build/windows/imports.txt $out/share/jollymaam/
    cp -r build/windows/tests $out/share/jollymaam/
    cp src/lib/*.h $out/include/jollymaam/
    cp src/lib/jieli_image/*.h $out/include/jollymaam/jieli_image/
  '';
}
