package org.totedb.ycsb;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.Map;

final class Audit {
  private Audit() {}

  static void run(String engine, String database, Path snapshot, Path baseline) throws IOException, NoSuchAlgorithmException {
    Map<String, byte[][]> previous = baseline == null ? Map.of() : readSnapshot(baseline);
    MessageDigest digest = MessageDigest.getInstance("SHA-256");
    MessageDigest keys = MessageDigest.getInstance("SHA-256");
    long changed = 0;
    long added = 0;
    long missing = previous.size();
    long bytes = 0;
    long rows = 0;
    String last = null;
    try (Connection connection = new Connection(engine, database, false, 60000);
         DataOutputStream output = new DataOutputStream(Files.newOutputStream(snapshot))) {
      NativeReply reply = Connection.require(connection.execute("SELECT ycsb_key," + String.join(",", SqlBinding.FIELDS)
          + " FROM usertable ORDER BY ycsb_key"));
      if (reply.columns.length != 11 || !reply.columns[0].equals("ycsb_key")) {
        throw new IllegalStateException("audit column count mismatch");
      }
      for (int index = 1; index < reply.columns.length; index++) {
        if (!reply.columns[index].equals(SqlBinding.FIELDS.get(index - 1))) {
          throw new IllegalStateException("audit column name mismatch");
        }
      }
      for (byte[][] row : reply.rows) {
        if (row.length != 11 || row[0] == null) {
          throw new IllegalStateException("audit row shape mismatch");
        }
        String key = new String(row[0], StandardCharsets.UTF_8);
        if (last != null && last.compareTo(key) >= 0) {
          throw new IllegalStateException("duplicate or unordered key " + key);
        }
        last = key;
        byte[][] before = previous.get(key);
        if (before == null) added++;
        else missing--;
        for (int index = 0; index < row.length; index++) {
          byte[] value = row[index];
          if (value == null || (index > 0 && value.length != 100)) {
            throw new IllegalStateException("invalid field length for " + key + " field " + index);
          }
          update(digest, value);
          output.writeInt(value.length);
          output.write(value);
          if (index == 0) update(keys, value);
          else {
            bytes += value.length;
            if (before != null && !Arrays.equals(before[index], value)) changed++;
          }
        }
        rows++;
      }
    }
    System.out.printf("{\"rows\":%d,\"payload_bytes\":%d,\"sha256\":\"%s\",\"keys_sha256\":\"%s\","
        + "\"changed_fields\":%d,\"added_rows\":%d,\"missing_rows\":%d,\"field_count\":10,\"field_bytes\":100}%n",
        rows, bytes, HexFormat.of().formatHex(digest.digest()), HexFormat.of().formatHex(keys.digest()), changed, added, missing);
  }

  private static void update(MessageDigest digest, byte[] value) {
    digest.update(new byte[] {(byte)(value.length >>> 24), (byte)(value.length >>> 16),
        (byte)(value.length >>> 8), (byte)value.length});
    digest.update(value);
  }

  private static Map<String, byte[][]> readSnapshot(Path path) throws IOException {
    Map<String, byte[][]> rows = new HashMap<>();
    try (DataInputStream input = new DataInputStream(Files.newInputStream(path))) {
      while (input.available() > 0) {
        byte[][] row = new byte[11][];
        for (int index = 0; index < row.length; index++) {
          int size = input.readInt();
          if (size < 0 || size > 1_000_000) throw new IOException("invalid snapshot cell length");
          row[index] = input.readNBytes(size);
          if (row[index].length != size) throw new EOFException("truncated snapshot");
        }
        rows.put(new String(row[0], StandardCharsets.UTF_8), row);
      }
    }
    return rows;
  }
}
