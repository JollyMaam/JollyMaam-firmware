package dev.cyclone.padtool;
public class NativeCore {
    static { System.loadLibrary("jollymaam-jni"); }
    public native String[] products(byte[] bytes);
    public native byte[] probe(NativeUsb transport, int kind);
    public native int run(byte[] bytes, String product, int kind, byte[] uid, NativeUsb transport);
}
