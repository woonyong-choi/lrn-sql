package org.totedb.ycsb;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.Vector;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.ArrayList;
import java.util.List;
import site.ycsb.ByteArrayByteIterator;
import site.ycsb.ByteIterator;
import site.ycsb.Status;

final class Smoke {
  private Smoke() {}

  static void run(String engine, String path) throws Exception {
    wire();
    try (Connection setup = new Connection(engine, path, true, 60000)) {
      Connection.require(setup.execute(SqlBinding.schema()));
    }
    SqlBinding db = binding(engine, path);
    byte[] special = new byte[] {0, (byte)255, 1, 0};
    Map<String, ByteIterator> inserted = values(special);
    require(db.insert("usertable", "user2", inserted).equals(Status.OK), "insert");
    require(db.insert("usertable", "user2", values(special)).equals(Status.ERROR), "duplicate rejected");
    require(db.insert("usertable", "user10", values(new byte[0])).equals(Status.OK), "empty blobs");
    require(db.insert("usertable", "사용자\uD834\uDD1E", values("다국어".getBytes(StandardCharsets.UTF_8))).equals(Status.OK), "unicode key");
    Map<String, ByteIterator> result = new HashMap<>();
    require(db.read("usertable", "user2", null, result).equals(Status.OK), "read");
    require(result.size() == 10 && Arrays.equals(result.get("field0").toArray(), special), "binary fields");
    require(db.update("usertable", "user2", Map.of("field3", new ByteArrayByteIterator(new byte[] {42}))).equals(Status.OK), "partial update");
    result.clear();
    db.read("usertable", "user2", Set.of("field0", "field3"), result);
    require(result.size() == 2 && result.get("field3").toArray()[0] == 42
        && Arrays.equals(result.get("field0").toArray(), special), "projection and untouched field");
    result.clear();
    require(db.read("usertable", "missing", null, result).equals(Status.NOT_FOUND), "missing read");
    require(db.update("usertable", "missing", Map.of("field0", new ByteArrayByteIterator(special))).equals(Status.NOT_FOUND), "missing update");
    require(db.delete("usertable", "missing").equals(Status.NOT_FOUND), "missing delete");
    Vector<HashMap<String, ByteIterator>> scan = new Vector<>();
    require(db.scan("usertable", "user1", 1, Set.of("field0"), scan).equals(Status.OK), "scan");
    require(scan.size() == 1 && scan.get(0).get("field0").toArray().length == 0, "lexical order and limit");
    scan.clear();
    db.scan("usertable", "user2", 20, Set.of("field3"), scan);
    require(scan.size() == 2, "short tail");
    require(db.connection.execute("THIS IS INVALID SQL").status == NativeReply.ERROR, "SQL error result");
    require(db.connection.exhausted == 0, "non-BUSY errors not retried");
    db.cleanup();
    db = binding(engine, path);
    result.clear();
    require(db.read("usertable", "사용자\uD834\uDD1E", null, result).equals(Status.OK), "reopen unicode");
    require(new String(result.get("field0").toArray(), StandardCharsets.UTF_8).equals("다국어"), "reopen value");
    require(db.delete("usertable", "user2").equals(Status.OK), "delete");
    result.clear();
    require(db.read("usertable", "user2", null, result).equals(Status.NOT_FOUND), "deleted read");
    db.cleanup();
    busy(engine, path);
    concurrent(engine, path);
    long descriptorsBefore = descriptors();
    for (int index = 0; index < 100; index++) {
      try (Connection connection = new Connection(engine, path, false, 60000)) {
        Connection.require(connection.execute("SELECT field0 FROM usertable WHERE ycsb_key='user10'"));
      }
    }
    require(descriptors() <= descriptorsBefore, "no file descriptor leak across 100 reopen cycles");
    System.out.println("{\"smoke\":\"pass\",\"engine\":\"" + engine + "\",\"concurrent_rows\":80,\"reopen_cycles\":100}");
  }

  private static long descriptors() {
    java.lang.management.OperatingSystemMXBean system = java.lang.management.ManagementFactory.getOperatingSystemMXBean();
    if (system instanceof com.sun.management.UnixOperatingSystemMXBean unix) return unix.getOpenFileDescriptorCount();
    throw new IllegalStateException("file descriptor audit requires UnixOperatingSystemMXBean");
  }

  private static void wire() {
    byte[] command = new byte[] {1, 6, 0, 0, 0, 'I', 'N', 'S', 'E', 'R', 'T', 1, 0, 0, 0, 0, 0, 0, 0};
    require(Native.decode(command).affected == 1, "little-endian affected rows");
    for (int size = 0; size < command.length; size++) {
      require(Native.decode(Arrays.copyOf(command, size)).status == NativeReply.ERROR, "truncated wire rejected");
    }
    require(Native.decode(Arrays.copyOf(command, command.length + 1)).status == NativeReply.ERROR, "trailing wire rejected");
    require(Native.decode(new byte[] {99}).status == NativeReply.ERROR, "unknown wire kind rejected");
    byte[] busy = new byte[] {2, 7, 0, 0, 0, 30, 0, 0, 0, 0, 0, 0, 0};
    require(Native.decode(busy).status == NativeReply.BUSY, "SQL BUSY inside TOTE_OK decoded");
    for (int code : new int[] {10, 12, 13}) {
      byte[] unsafe = new byte[] {2, 6, 0, 0, 0, (byte)code, 0, 0, 0, 0, 0, 0, 0};
      require(Native.decode(unsafe).status == NativeReply.ERROR, "unsafe errors are not BUSY");
    }
  }

  private static void busy(String engine, String path) throws Exception {
    try (Connection holder = new Connection(engine, path, false, 60000);
         Connection waiter = new Connection(engine, path, false, 0)) {
      long transaction = holder.holdWriter();
      try {
        NativeReply reply = waiter.execute("UPDATE usertable SET field0=X'2A' WHERE ycsb_key='user10'");
        require(reply.status == NativeReply.BUSY && waiter.exhausted == 1 && waiter.retries == 0, "bounded BUSY exhaustion");
      } finally {
        holder.releaseWriter(transaction);
      }
      NativeReply unchanged = Connection.require(waiter.execute("SELECT field0 FROM usertable WHERE ycsb_key='user10'"));
      require(unchanged.rows[0][0].length == 0, "BUSY write not applied");
      Connection.require(waiter.execute("UPDATE usertable SET field0=X'2A' WHERE ycsb_key='user10'"));
    }
    if (engine.equals("sqlite")) {
      try (Connection reader = new Connection(engine, path, false, 60000);
           Connection writer = new Connection(engine, path, false, 0)) {
        Connection.require(reader.execute("BEGIN"));
        Connection.require(reader.execute("SELECT field0 FROM usertable WHERE ycsb_key='user10'"));
        NativeReply blocked = writer.execute("UPDATE usertable SET field0=X'FF' WHERE ycsb_key='user10'");
        require(blocked.status == NativeReply.BUSY, "SQLite commit blocked by reader");
        Connection.require(reader.execute("ROLLBACK"));
        NativeReply unchanged = Connection.require(writer.execute("SELECT field0 FROM usertable WHERE ycsb_key='user10'"));
        require(Arrays.equals(unchanged.rows[0][0], new byte[] {42}), "SQLite BUSY finalize rolled back write");
      }
    }
    try (Connection holder = new Connection(engine, path, false, 60000);
         Connection waiter = new Connection(engine, path, false, 60000)) {
      long transaction = holder.holdWriter();
      ExecutorService pool = Executors.newSingleThreadExecutor();
      try {
        Future<NativeReply> result = pool.submit(() -> waiter.execute("UPDATE usertable SET field0=X'2B' WHERE ycsb_key='user10'"));
        Thread.sleep(100);
        holder.releaseWriter(transaction);
        transaction = 0;
        require(result.get().status == NativeReply.OK && waiter.retries > 0 && waiter.exhausted == 0, "BUSY retry success");
      } finally {
        if (transaction != 0) holder.releaseWriter(transaction);
        pool.shutdownNow();
      }
    }
  }

  private static void concurrent(String engine, String path) throws Exception {
    ExecutorService pool = Executors.newFixedThreadPool(4);
    CountDownLatch ready = new CountDownLatch(4);
    CountDownLatch start = new CountDownLatch(1);
    List<Future<?>> futures = new ArrayList<>();
    try {
      for (int worker = 0; worker < 4; worker++) {
        int workerId = worker;
        futures.add(pool.submit(() -> {
          ready.countDown();
          start.await();
          SqlBinding db = binding(engine, path);
          try {
            for (int index = 0; index < 20; index++) {
              String key = "worker" + workerId + "/" + index;
              byte[] expected = new byte[] {(byte)workerId, (byte)index};
              require(db.insert("usertable", key, values(expected)).equals(Status.OK), "concurrent insert");
              Map<String, ByteIterator> result = new HashMap<>();
              require(db.read("usertable", key, null, result).equals(Status.OK), "concurrent read");
              require(result.size() == 10, "concurrent field count");
              for (ByteIterator value : result.values()) require(Arrays.equals(value.toArray(), expected), "concurrent value");
            }
          } finally {
            db.cleanup();
          }
          return null;
        }));
      }
      ready.await();
      start.countDown();
      for (Future<?> future : futures) future.get();
    } finally {
      pool.shutdownNow();
    }
    try (Connection connection = new Connection(engine, path, false, 60000)) {
      NativeReply reply = Connection.require(connection.execute("SELECT ycsb_key FROM usertable WHERE ycsb_key>='worker' ORDER BY ycsb_key"));
      long workers = Arrays.stream(reply.rows).filter(row -> new String(row[0], StandardCharsets.UTF_8).startsWith("worker")).count();
      require(workers == 80, "all concurrent writes persisted");
    }
  }

  private static SqlBinding binding(String engine, String path) throws Exception {
    SqlBinding binding = new SqlBinding();
    Properties properties = new Properties();
    properties.setProperty("engine", engine);
    properties.setProperty("dbpath", path);
    binding.setProperties(properties);
    binding.init();
    return binding;
  }

  private static Map<String, ByteIterator> values(byte[] value) {
    Map<String, ByteIterator> result = new HashMap<>();
    for (String field : SqlBinding.FIELDS) result.put(field, new ByteArrayByteIterator(value.clone()));
    return result;
  }

  private static void require(boolean condition, String operation) {
    if (!condition) throw new IllegalStateException("smoke failed: " + operation);
  }
}
