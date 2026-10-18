package org.totedb.ycsb;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.Vector;
import site.ycsb.ByteArrayByteIterator;
import site.ycsb.ByteIterator;
import site.ycsb.DB;
import site.ycsb.DBException;
import site.ycsb.Status;

public final class SqlBinding extends DB {
  static final List<String> FIELDS = List.of("field0", "field1", "field2", "field3", "field4",
      "field5", "field6", "field7", "field8", "field9");
  Connection connection;

  @Override
  public void init() throws DBException {
    try {
      connection = new Connection(getProperties().getProperty("engine"),
          getProperties().getProperty("dbpath"), false,
          Long.parseLong(getProperties().getProperty("busyretryms", "60000")));
    } catch (RuntimeException error) {
      throw new DBException(error);
    }
  }

  @Override
  public void cleanup() throws DBException {
    if (connection != null) {
      try {
        connection.close();
        connection.printMetrics();
      } catch (RuntimeException error) {
        throw new DBException(error);
      }
    }
  }

  @Override
  public Status read(String table, String key, Set<String> fields, Map<String, ByteIterator> result) {
    checkTable(table);
    List<String> selected = selectFields(fields);
    NativeReply reply = connection.execute("SELECT " + String.join(",", selected)
        + " FROM usertable WHERE ycsb_key=?", new byte[][] {Connection.utf8(key)}, new boolean[] {true});
    if (reply.status != NativeReply.OK) {
      return failure(reply);
    }
    if (reply.rows.length == 0) {
      return Status.NOT_FOUND;
    }
    materialize(reply, reply.rows[0], result);
    return Status.OK;
  }

  @Override
  public Status scan(String table, String startKey, int count, Set<String> fields,
      Vector<HashMap<String, ByteIterator>> result) {
    checkTable(table);
    if (count <= 0) {
      throw new IllegalArgumentException("scan count must be positive");
    }
    List<String> selected = selectFields(fields);
    NativeReply reply = connection.execute("SELECT " + String.join(",", selected)
        + " FROM usertable WHERE ycsb_key>=? ORDER BY ycsb_key LIMIT " + count,
        new byte[][] {Connection.utf8(startKey)}, new boolean[] {true});
    if (reply.status != NativeReply.OK) {
      return failure(reply);
    }
    for (byte[][] row : reply.rows) {
      HashMap<String, ByteIterator> record = new HashMap<>();
      materialize(reply, row, record);
      result.add(record);
    }
    return Status.OK;
  }

  @Override
  public Status update(String table, String key, Map<String, ByteIterator> values) {
    checkTable(table);
    List<String> fields = selectFields(values.keySet());
    byte[][] parameters = new byte[fields.size() + 1][];
    boolean[] text = new boolean[parameters.length];
    List<String> assignments = new ArrayList<>();
    for (int index = 0; index < fields.size(); index++) {
      assignments.add(fields.get(index) + "=?");
      parameters[index] = values.get(fields.get(index)).toArray();
    }
    parameters[fields.size()] = Connection.utf8(key);
    text[fields.size()] = true;
    return mutation(connection.execute("UPDATE usertable SET " + String.join(",", assignments)
        + " WHERE ycsb_key=?", parameters, text));
  }

  @Override
  public Status insert(String table, String key, Map<String, ByteIterator> values) {
    checkTable(table);
    if (!values.keySet().equals(Set.copyOf(FIELDS))) {
      throw new IllegalArgumentException("insert requires field0 through field9");
    }
    byte[][] parameters = new byte[11][];
    boolean[] text = new boolean[11];
    parameters[0] = Connection.utf8(key);
    text[0] = true;
    for (int index = 0; index < FIELDS.size(); index++) {
      parameters[index + 1] = values.get(FIELDS.get(index)).toArray();
    }
    return mutation(connection.execute("INSERT INTO usertable (ycsb_key," + String.join(",", FIELDS)
        + ") VALUES (?,?,?,?,?,?,?,?,?,?,?)", parameters, text));
  }

  @Override
  public Status delete(String table, String key) {
    checkTable(table);
    return mutation(connection.execute("DELETE FROM usertable WHERE ycsb_key=?",
        new byte[][] {Connection.utf8(key)}, new boolean[] {true}));
  }

  static String schema() {
    List<String> columns = new ArrayList<>();
    columns.add("ycsb_key TEXT PRIMARY KEY");
    for (String field : FIELDS) {
      columns.add(field + " BLOB NOT NULL");
    }
    return "CREATE TABLE usertable (" + String.join(",", columns) + ")";
  }

  private static void checkTable(String table) {
    if (!table.equals("usertable")) {
      throw new IllegalArgumentException("table must be usertable");
    }
  }

  private static List<String> selectFields(Set<String> fields) {
    if (fields == null) {
      return FIELDS;
    }
    if (fields.isEmpty() || !FIELDS.containsAll(fields)) {
      throw new IllegalArgumentException("invalid field selection " + fields);
    }
    String[] selected = fields.toArray(String[]::new);
    Arrays.sort(selected);
    return List.of(selected);
  }

  private static void materialize(NativeReply reply, byte[][] row, Map<String, ByteIterator> result) {
    for (int index = 0; index < reply.columns.length; index++) {
      if (row[index] == null) {
        throw new IllegalStateException("unexpected NULL field");
      }
      result.put(reply.columns[index], new ByteArrayByteIterator(row[index]));
    }
  }

  private static Status mutation(NativeReply reply) {
    if (reply.status != NativeReply.OK) {
      return failure(reply);
    }
    return reply.affected == 1 ? Status.OK : Status.NOT_FOUND;
  }

  private static Status failure(NativeReply reply) {
    System.err.println("BINDING_ERROR code=" + reply.code + " message=" + reply.error);
    return Status.ERROR;
  }
}
