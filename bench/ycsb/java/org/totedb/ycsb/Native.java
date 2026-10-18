package org.totedb.ycsb;

final class Native {
  static {
    System.load(System.getProperty("ycsb.native"));
  }

  private Native() {}

  static native long open(int engine, byte[] path, boolean create);
  static native NativeReply decode(byte[] encoded);
  static native long begin(long handle);
  static native int rollback(long handle, long transaction);
  static native int close(long handle);
  static native NativeReply execute(long handle, byte[] sql, byte[][] values, boolean[] text);
  static native String[] compileOptions();
  static native String version(int engine);
}
