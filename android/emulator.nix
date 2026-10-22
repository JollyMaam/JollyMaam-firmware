# Keep the emulator/image out of ordinary APK build environments.
import ./shell.nix { withEmulator = true; }
