# Bug: NOT NULL Constraint Not Enforced for Nested Types (STRUCT, LIST, MAP)

## Reproduction

```sql
ATTACH ':memory:' AS dl (TYPE ducklake);
CREATE TABLE dl.tbl(id INT, s STRUCT(a INT, b INT) NOT NULL);
INSERT INTO dl.tbl VALUES (1, NULL);  -- Should fail, but succeeds
```

## Root Cause

DuckLake enforces NOT NULL constraints via **stats-based validation** — it checks
the `null_count` statistic for each column after data is written. This differs from
DuckDB's native catalog, which validates per-row using `VectorOperations::HasNull`
during append.

The bug affects **both** DuckLake insert paths:

### Path 1: Inlined Data (`ducklake_inline_data.cpp`)

This path is used for small inserts (default: <= 10 rows), which is the exact
reproduction case.

In `UpdateStats()` (line 247), when the column type is nested:

```cpp
if (type.IsNested() && type.id() != LogicalTypeId::VARIANT) {
    switch (data.GetType().id()) {
    case LogicalTypeId::STRUCT: {
        auto &children = StructVector::GetEntries(data);
        for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
            UpdateStats(column_stats.children, child_idx, *children[child_idx], ...);
        }
        break;
    }
    // ... LIST, MAP similar
    }
    return;  // <-- BUG: returns without computing null count for the column itself
}
auto new_stats = GetVectorStats(data, row_count);  // <-- never reached for nested types
```

The function **recurses into children** and **returns immediately** without ever
calling `GetVectorStats()` on the top-level vector. This means
`column_stats.stats.null_count` stays at 0, and the NOT NULL check at line 337
always passes:

```cpp
if (column_stats.stats.null_count > 0) {  // always false for nested types
    ...
    throw ConstraintException("NOT NULL constraint failed: ...");
}
```

### Path 2: File-Based Insert (`ducklake_insert.cpp`)

This path is used for larger inserts (or when inlining is disabled). Stats come
from the Parquet writer's output.

In `AddWrittenFiles()` (line 182):

```cpp
if (column_stats.null_count > 0 && column_names.size() == 1) {
    if (global_state.not_null_fields.count(column_names[0])) {
        throw ConstraintException("NOT NULL constraint failed: ...");
    }
}
```

The guard `column_names.size() == 1` limits the check to top-level **primitive**
columns. For a struct column `s STRUCT(a INT, b INT)`, the Parquet writer reports
stats for leaf columns only:
- `s.a` → `column_names = ["s", "a"]` (size 2) → **skipped**
- `s.b` → `column_names = ["s", "b"]` (size 2) → **skipped**

The Parquet format doesn't store struct-level statistics, so there is no entry for
`s` itself. Even if there were, the `column_names.size() == 1` check was designed
for simple columns and would need adjustment.

## Affected Types

All nested types: **STRUCT**, **LIST**, **MAP** (and potentially arrays/unions).

## Proposed Fix

### Fix 1: Inlined Data Path (covers the immediate repro)

In `UpdateStats()`, before the early `return` for nested types, compute the null
count for the top-level vector using its validity mask:

```cpp
if (type.IsNested() && type.id() != LogicalTypeId::VARIANT) {
    // Compute null count for the nested column itself
    UnifiedVectorFormat format;
    data.ToUnifiedFormat(row_count, format);
    DuckLakeColumnStats nested_stats(type);
    nested_stats.has_null_count = true;
    for (idx_t i = 0; i < row_count; i++) {
        if (!format.validity.RowIsValid(format.sel->get_index(i))) {
            nested_stats.null_count++;
        }
    }
    if (column_stats.has_stats) {
        column_stats.stats.MergeNullCount(nested_stats);
    } else {
        column_stats.stats = std::move(nested_stats);
        column_stats.has_stats = true;
    }

    // Then recurse into children as before
    switch (data.GetType().id()) { ... }
    return;
}
```

### Fix 2: File-Based Path

Two possible approaches:

**Option A — Validate before COPY:** Add an intermediate validation operator in
the physical plan that checks NOT NULL constraints on nested columns before data
flows to the Parquet writer. This is similar to DuckDB's native
`VerifyNotNullConstraint`.

**Option B — Infer from stats:** After processing all column stats from Parquet,
for each NOT NULL nested column that has no direct stats entry, explicitly flag an
error. This is trickier because we'd need to distinguish "no nulls" from "no stats
reported."

Recommendation: **Option A** is more robust and follows the established DuckDB
pattern.

## Test Plan

- `test/sql/constraints/not_null_nested_types.test` — Tests NOT NULL for STRUCT,
  LIST, MAP via the default (inlined) path
- `test/sql/constraints/not_null_nested_types_file_path.test` — Same tests with
  `DATA_INLINING_ROW_LIMIT 0` to exercise the file-based path
