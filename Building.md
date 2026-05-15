# Building Dabble

## Default build (official DuckDB release)

Downloads DuckDB v1.5.2 automatically. No setup needed.

```bash
git clone https://github.com/yourname/dabble
cd dabble
cmake -B build
cmake --build build -j
./build/bin/dabble --help
```

---

## Building with a custom DuckDB (extensions baked in)

If you have custom DuckDB extensions that are painful to load at runtime
(signing issues, autoload not working, specific commit requirements),
the cleanest solution is to build Dabble against your own `libduckdb.so`
that has those extensions compiled in.

### Step 1 — Build DuckDB with your extensions

Clone DuckDB at the commit your extensions target:

```bash
git clone https://github.com/duckdb/duckdb
cd duckdb
git checkout <your-target-commit>
```

Build with your extensions. The extensions need to be in `extension/` or
specified via cmake. Example with out-of-tree extensions:

```bash
cmake -B build/release \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_UNITTESTS=OFF \
    -DBUILD_SHELL=OFF \
    -DDUCKDB_EXTENSION_CONFIGS="/path/to/your/extension/.duckdb_extension.yml"
cmake --build build/release -j
```

After building you should have:
```
duckdb/build/release/src/libduckdb.so   ← the library
duckdb/src/include/duckdb.h             ← the header
```

### Step 2 — Build Dabble against your library

Two CMake variables control the custom build:

| Variable | Description |
|---|---|
| `DUCKDB_LOCAL` | Path to the directory containing `libduckdb.so` |
| `DUCKDB_INCLUDE` | Path to the directory containing `duckdb.h` (auto-detected if not set) |

```bash
cd dabble
cmake -B build \
    -DDUCKDB_LOCAL=/path/to/duckdb/build/release/src \
    -DDUCKDB_INCLUDE=/path/to/duckdb/src/include
cmake --build build -j
```

### Step 3 — Verify extensions are available

```bash
# Table functions need FROM clause:
cat > /tmp/test_ext.dabble << 'EOF2'
let result = SELECT * FROM your_extension_function([1,2,3]);
result;
EOF2
./build/bin/dabble /tmp/test_ext.dabble
```

If your extensions are baked into `libduckdb.so`, no `LOAD` statements
are needed in your scripts.

### Troubleshooting

**`Cannot find duckdb.h`**

Run `find /path/to/duckdb -name "duckdb.h"` and pass the containing
directory explicitly:

```bash
cmake -B build \
    -DDUCKDB_LOCAL=.../duckdb/build/release/src \
    -DDUCKDB_INCLUDE=.../duckdb/src/include
```

**`libduckdb.so: cannot open shared object file`** at runtime

The `.so` needs to be findable at runtime. Either:

```bash
# Option A — set LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/path/to/duckdb/build/release/src:$LD_LIBRARY_PATH
./build/bin/dabble script.dabble

# Option B — copy the .so next to the binary
cp /path/to/duckdb/build/release/src/libduckdb.so ./build/bin/

# Option C — embed the rpath at build time (add to CMakeLists.txt)
set_target_properties(dabble PROPERTIES
    BUILD_RPATH "/path/to/duckdb/build/release/src"
    INSTALL_RPATH "/path/to/duckdb/build/release/src"
)
```

**Extensions not found even with custom build**

Extensions may still be separate `.duckdb_extension` files rather than
compiled into `libduckdb.so`. In that case, `LOAD` them explicitly at
the top of your script:

```sql
LOAD '/absolute/path/to/your_extension.duckdb_extension';
```

---

## Building DuckDB from source at a specific commit (via CMake)

If you want CMake to fetch and build DuckDB from source automatically:

```bash
cmake -B build \
    -DDUCKDB_COMMIT=abc123def456 \
    -DDUCKDB_EXTENSIONS="ext1;ext2"
cmake --build build -j
```

This pulls the exact commit from GitHub and builds DuckDB + extensions
together with Dabble. Slower first build (~5 min) but fully reproducible.

---

## Running tests

```bash
ctest --test-dir build --output-on-failure

# Run a single test:
ctest --test-dir build -R test_vals --output-on-failure

# Verbose output:
ctest --test-dir build -V
```

Tests are Dabble scripts in `tests/test_*.dabble`. Each uses
`check ... else fail` assertions and exits non-zero on failure.