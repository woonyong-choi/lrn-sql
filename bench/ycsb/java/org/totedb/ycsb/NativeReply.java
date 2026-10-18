package org.totedb.ycsb;

final class NativeReply {
  static final int OK = 0;
  static final int BUSY = 1;
  static final int ERROR = 2;

  final int status;
  final int code;
  final String error;
  final String[] columns;
  final byte[][][] rows;
  final long affected;

  NativeReply(int status, int code, String error, String[] columns, byte[][][] rows, long affected) {
    this.status = status;
    this.code = code;
    this.error = error;
    this.columns = columns;
    this.rows = rows;
    this.affected = affected;
  }
}
