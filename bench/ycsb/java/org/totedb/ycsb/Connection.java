package org.totedb.ycsb;

import java.nio.charset.StandardCharsets;
import java.util.concurrent.locks.LockSupport;

final class Connection implements AutoCloseable {
  private static final long INITIAL_DELAY_NS = 500_000;
  private static final long MAX_DELAY_NS = 20_000_000;

  final int engine;
  final long budgetNanos;
  long setupAttempts;
  long setupRetries;
  long attempts;
  long retries;
  long exhausted;
  long openRetries;
  long sqlBytes;
  long inputBytes;
  long outputBytes;
  long resultRows;
  private long handle;

  Connection(String engineName, String path, boolean create, long retryMillis) {
    engine = switch (engineName) {
      case "totedb" -> 0;
      case "sqlite" -> 1;
      default -> throw new IllegalArgumentException("unknown engine " + engineName);
    };
    if (retryMillis < 0 || retryMillis > 60000) {
      throw new IllegalArgumentException("retry budget must be between 0 and 60000 ms");
    }
    budgetNanos = retryMillis * 1_000_000;
    long started = System.nanoTime();
    long delay = INITIAL_DELAY_NS;
    while (true) {
      handle = Native.open(engine, utf8(path), create);
      if (handle > 0) {
        break;
      }
      boolean busy = engine == 0 ? handle == -104 || handle == -4 : handle == -5;
      if (!busy || System.nanoTime() - started >= budgetNanos) {
        throw new IllegalStateException("open failed with code " + handle);
      }
      pause(Math.min(delay, budgetNanos - (System.nanoTime() - started)));
      if (System.nanoTime() - started >= budgetNanos) {
        throw new IllegalStateException("open retry budget exhausted with code " + handle);
      }
      openRetries++;
      delay = Math.min(delay * 2, MAX_DELAY_NS);
    }
    try {
      if (engine == 1) {
        require(execute("PRAGMA busy_timeout=0"));
        require(execute("PRAGMA page_size=8192"));
        require(execute("PRAGMA journal_mode=DELETE"));
        require(execute("PRAGMA synchronous=FULL"));
        require(execute("PRAGMA fullfsync=OFF"));
        require(execute("PRAGMA cache_size=256"));
        require(execute("PRAGMA mmap_size=0"));
      }
      setupAttempts = attempts;
      setupRetries = retries;
      retries = 0;
      attempts = 0;
      sqlBytes = 0;
      inputBytes = 0;
      outputBytes = 0;
      resultRows = 0;
    } catch (RuntimeException error) {
      close();
      throw error;
    }
  }

  @Override
  public void close() {
    if (handle == 0) {
      return;
    }
    int status = Native.close(handle);
    if (status != 0) {
      throw new IllegalStateException("close failed with code " + status);
    }
    handle = 0;
  }

  long holdWriter() {
    long transaction = Native.begin(handle);
    if (transaction <= 0) throw new IllegalStateException("writer lock failed " + transaction);
    return transaction;
  }

  void releaseWriter(long transaction) {
    int status = Native.rollback(handle, transaction);
    if (status != 0) throw new IllegalStateException("writer rollback failed " + status);
  }

  NativeReply execute(String sql) {
    return execute(sql, new byte[0][], new boolean[0]);
  }

  NativeReply execute(String sql, byte[][] values, boolean[] text) {
    if (handle == 0) {
      throw new IllegalStateException("connection is closed");
    }
    byte[] encodedSql = utf8(sql);
    long started = System.nanoTime();
    long delay = INITIAL_DELAY_NS;
    while (true) {
      attempts++;
      sqlBytes += encodedSql.length;
      for (byte[] value : values) {
        inputBytes += value.length;
      }
      NativeReply reply = Native.execute(handle, encodedSql, values, text);
      if (reply.status != NativeReply.BUSY) {
        for (byte[][] row : reply.rows) {
          resultRows++;
          for (byte[] cell : row) {
            outputBytes += cell == null ? 0 : cell.length;
          }
        }
        return reply;
      }
      if (System.nanoTime() - started >= budgetNanos) {
        exhausted++;
        return reply;
      }
      pause(Math.min(delay, budgetNanos - (System.nanoTime() - started)));
      if (System.nanoTime() - started >= budgetNanos) {
        exhausted++;
        return reply;
      }
      retries++;
      delay = Math.min(delay * 2, MAX_DELAY_NS);
    }
  }

  void printMetrics() {
    System.err.printf("BINDING setup_attempts=%d setup_retries=%d attempts=%d retries=%d exhausted=%d open_retries=%d sql_bytes=%d input_bytes=%d output_bytes=%d result_rows=%d%n",
        setupAttempts, setupRetries, attempts, retries, exhausted, openRetries, sqlBytes, inputBytes, outputBytes, resultRows);
  }

  static NativeReply require(NativeReply reply) {
    if (reply.status != NativeReply.OK) {
      throw new IllegalStateException("sql failed with code " + reply.code + ": " + reply.error);
    }
    return reply;
  }

  static byte[] utf8(String value) {
    if (value.indexOf('\0') >= 0) {
      throw new IllegalArgumentException("embedded NUL in string");
    }
    return value.getBytes(StandardCharsets.UTF_8);
  }

  private static void pause(long nanos) {
    LockSupport.parkNanos(nanos);
    if (Thread.currentThread().isInterrupted()) {
      throw new IllegalStateException("retry interrupted");
    }
  }
}
