package dev.cyclone.padtool;
public abstract class NativeUsb {
    public int scsi(byte[] cdb, int direction, byte[] data, int timeout) { throw new UnsupportedOperationException(); }
    public int hidWrite(byte[] data) { throw new UnsupportedOperationException(); }
    public int hidRead(byte[] data, int timeout) { throw new UnsupportedOperationException(); }
    public int featureSet(byte[] data) { throw new UnsupportedOperationException(); }
    public int featureSize() { return 0; }
    public void waitMillis(int ms) {}
    public boolean reconnect(byte[] uid) { return false; }
    public void onProgress(long done, long total) {}
}
