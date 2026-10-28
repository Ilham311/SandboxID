package android.database;

/**
 * Host stand-in for {@code android.database.MatrixCursor}, covering exactly the
 * surface the M_GSF_CURSOR hook uses: the {@code (String[], int)} constructor and
 * {@code addRow(Object[])}.
 *
 * <p>The real class is reached through {@code Class.forName} inside the callback,
 * because the callback is compiled without android.jar. That makes the reflective
 * call shape part of the contract under test: a constructor or method signature
 * that drifts here is a hook that silently falls through to the real
 * ContentResolver on device, which is the er:103 this fix exists to remove.
 */
public class MatrixCursor {

    private final String[] columns;
    private final java.util.List<Object[]> rows = new java.util.ArrayList<Object[]>();

    public MatrixCursor(String[] columnNames, int initialCapacity) {
        this.columns = columnNames.clone();
    }

    public void addRow(Object[] columnValues) {
        if (columnValues.length != columns.length) {
            throw new IllegalArgumentException("columnNames.length = " + columns.length
                    + ", columnValues.length = " + columnValues.length);
        }
        rows.add(columnValues.clone());
    }

    // ---- accessors the test reads; the real Cursor has these too ----

    public int getColumnCount() {
        return columns.length;
    }

    public int getCount() {
        return rows.size();
    }

    public String getColumnName(int index) {
        return columns[index];
    }

    public String getString(int index) {
        return (String) rows.get(0)[index];
    }
}
