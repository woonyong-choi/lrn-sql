package org.totedb.ycsb;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

public final class Tool {
  private Tool() {}

  public static void main(String[] args) throws Exception {
    switch (args[0]) {
      case "setup" -> setup(args[1], args[2]);
      case "audit" -> Audit.run(args[1], args[2], Path.of(args[3]), args.length == 5 ? Path.of(args[4]) : null);
      case "smoke" -> Smoke.run(args[1], args[2]);
      case "version" -> System.out.printf("{\"totedb\":\"%s\",\"sqlite\":\"%s\",\"sqlite_compile_options\":[\"%s\"]}%n",
          Native.version(0), Native.version(1), String.join("\",\"", Native.compileOptions()));
      default -> throw new IllegalArgumentException("unknown command " + args[0]);
    }
  }

  private static void setup(String engine, String path) {
    if (Files.exists(Path.of(path))) throw new IllegalArgumentException("database already exists " + path);
    try (Connection connection = new Connection(engine, path, true, 60000)) {
      Connection.require(connection.execute(SqlBinding.schema()));
      if (engine.equals("sqlite")) {
        List<String> pragmas = List.of("page_size", "journal_mode", "synchronous", "fullfsync", "busy_timeout", "cache_size", "mmap_size");
        List<String> expected = List.of("8192", "delete", "2", "0", "0", "256", "0");
        for (int index = 0; index < pragmas.size(); index++) {
          NativeReply reply = Connection.require(connection.execute("PRAGMA " + pragmas.get(index)));
          String value = new String(reply.rows[0][0], StandardCharsets.UTF_8);
          if (!value.equals(expected.get(index))) throw new IllegalStateException("unexpected PRAGMA " + pragmas.get(index) + "=" + value);
          System.out.println(pragmas.get(index) + "=" + value);
        }
      }
      System.out.println("native_version=" + Native.version(connection.engine));
    }
  }
}
