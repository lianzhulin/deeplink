---
name: "controller-driver-ut"
description: "Generates a complete controller driver package: JSON interface definition, C header, driver code, UT tests, and 100% line+branch coverage report. Invoke when user needs to create a hardware controller driver with register-level abstraction and full unit test coverage."
---

# Controller Driver + UT Coverage Generator

This skill generates a complete, production-ready controller driver package from a hardware register specification, including full unit tests with 100% statement and 100% branch coverage.

## When to Invoke

- User needs to create a hardware module/controller driver based on register interface
- User wants to define a controller interface in JSON and generate C code
- User needs unit tests with code coverage report for a C driver
- User asks for "controller driver + UT + coverage" workflow
- User wants to achieve 100% line coverage and 100% branch coverage for driver code

## Workflow Overview

```
JSON 接口定义 → C 头文件生成 → 驱动代码 → UT 测试 → 覆盖率报告(双100%)
```

### Step 1: JSON Interface Definition

Create a `controller_interface.json` file describing the controller:

```json
{
  "name": "<ModuleName>",
  "base_addr": "0x<BASE_ADDR>",
  "version": "<version>",
  "registers": [
    {
      "name": "<REG_NAME>",
      "offset": "0x<OFFSET>",
      "access": "RW | RO",
      "fields": [
        {
          "name": "<FIELD_NAME>",
          "bit": <bit_position>,
          "width": <bit_width>,
          "reset": <reset_value>,
          "desc": "<description>"
        }
      ]
    }
  ]
}
```

**Rules:**
- Each register has a name, offset, access mode (RW=read-write, RO=read-only)
- Each field has bit position, width, reset value, and description
- At least one RW register (for control) and one RO register (for status) is recommended

### Step 2: Generate C Header File

Generate `<controller_interface>.h` from JSON with:

- `#define <REG>_ADDR` — register absolute address (base_addr + offset)
- `#define <REG>_<FIELD>_Msk` — bit mask: `((1U << width) - 1) << bit`
- `#define <REG>_<FIELD>_Pos` — bit position
- `#define <REG>_<FIELD>_Rst` — reset value
- `typedef enum` for multi-value fields (e.g., state enums)
- Access control comments (`/* RW */` / `/* RO */`)

### Step 3: Driver Implementation

Create `<module>_driver.h` and `<module>_driver.c`:

#### Driver Architecture (pluggable I/O)

```c
/* Hook function types for register access */
typedef uint32_t (*reg_read_fn_t)(uint32_t addr);
typedef void     (*reg_write_fn_t)(uint32_t addr, uint32_t value);

/* Register custom I/O hooks */
void module_driver_register_io(reg_read_fn_t read_fn, reg_write_fn_t write_fn);

/* MMIO mirror mode (for safe user-space testing) */
void module_driver_set_mmio_mirror(uint32_t *mirror);
```

#### Key Design Decisions

1. **Default I/O**: `volatile uint32_t*` pointer access to MMIO address
2. **Mirror mode**: When `g_mirror != NULL`, read/write goes to a user-space array (safe for UT, no segfault)
3. **Hook injection**: Allow custom read/write functions for platform abstraction
4. **Address validation**: Internal `default_reg_index()` returns 0/1 for valid regs, -1 for illegal addresses
5. **RO register protection**: Write to RO register is silently ignored or returns error

#### Driver API Functions

- `module_driver_init()` — Initialize driver, set default I/O
- `module_enable()` / `module_disable()` — Control module via RW register
- `module_get_state()` — Query status via RO register
- `module_soft_reset()` — Trigger soft reset
- `module_driver_ut_probe_default_io(int mode)` — UT-only function to cover all default I/O branches:
  - `mode=0`: mirror path (g_mirror != NULL), covers all if/else in default_reg_read/write
  - `mode=1`: volatile path (g_mirror == NULL), requires test to mmap the address first

### Step 4: Unit Tests

Create `test_<module>_driver.c`:

#### Test Framework

Use a lightweight assert-based framework (no external dependency):

```c
#define TEST_CASE(name) static void name(void)
#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); g_fail++; } \
    else { g_pass++; } \
} while(0)
```

#### Test Coverage Strategy

| Test Case | What It Covers |
|-----------|---------------|
| `init_reset_values` | Driver init, default register values |
| `enable_disable` | module_enable/disable normal + early-return (already enabled/disabled) |
| `soft_reset` | Soft reset normal + early-return (already in reset) |
| `module_state_read` | All enum cases in switch + default branch |
| `status_reg_never_written` | RO register write protection |
| `default_io_mirror_mode` | Mirror read/write for all registers |
| `default_io_probe_all_branches` | **Critical**: calls `probe(0)` + `probe(1)` with mmap to cover all default_reg_read/write branches |
| `bad_addr_paths` | Illegal address handling (idx=-1 path) |

#### mmap MAP_FIXED for Volatile Branch Coverage

To safely access the real MMIO address (e.g., `0x40000000`) in user-space:

```c
#include <sys/mman.h>
#include <unistd.h>

static void *g_mmio_base = NULL;
static size_t g_page_size = 0;

static int ut_map_mmio_page(void) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return -1;
    g_page_size = (size_t)ps;
    g_mmio_base = mmap((void*)(uintptr_t)BASE_ADDR, g_page_size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                       -1, 0);
    if (g_mmio_base == MAP_FAILED) { g_mmio_base = NULL; return -1; }
    memset(g_mmio_base, 0, g_page_size);
    return 0;
}

static void ut_unmap_mmio_page(void) {
    if (g_mmio_base) { munmap(g_mmio_base, g_page_size); g_mmio_base = NULL; }
}
```

This ensures the `g_mirror == NULL` branch in `default_reg_read/write` is covered without segfault.

### Step 5: Coverage Report (100% Line + 100% Branch)

#### Compilation (must use -O0 + --coverage)

```bash
gcc -std=c99 -Wall -Wextra -Werror -I. -O0 --coverage \
    -o test_<module>_driver_cov test_<module>_driver.c <module>_driver.c
```

#### Run + Collect + Generate

```bash
# Run UT
./test_<module>_driver_cov

# Collect coverage (enable branch coverage)
lcov --capture --directory . --output-file coverage.info --rc branch_coverage=1

# Generate HTML (line + branch displayed separately)
genhtml coverage.info --branch-coverage --output-directory coverage_html --show-details --legend --frames

# Extract driver-only coverage (exclude test file)
lcov --remove coverage.info "*test_*" --output-file /tmp/driver_only.info --rc branch_coverage=1
lcov --summary /tmp/driver_only.info --rc branch_coverage=1
```

#### run_ut.sh — One-Click Script

Generate a `run_ut.sh` that automates:
1. Tool check (gcc, gcov, lcov, genhtml)
2. Clean old artifacts
3. Compile with `--coverage -O0`
4. Run UT binary
5. `lcov --capture --rc branch_coverage=1`
6. `lcov --remove` to isolate driver file
7. `genhtml --branch-coverage` for both overall + driver-only reports
8. Terminal summary with Line/Func/Branch displayed separately (Chinese labels)
9. Target validation: exit 0 if Line==100% AND Branch==100%, else exit 3

#### Coverage Target

The driver file (`<module>_driver.c`) must achieve:
- **Line Coverage (语句覆盖率)**: 100.0%
- **Branch Coverage (分支覆盖率)**: 100.0%
- **Function Coverage (函数覆盖率)**: 100.0%

Both Line and Branch must be displayed **separately** in the HTML report (via `genhtml --branch-coverage`).

### Step 6: Documentation

Update README.md with:
- File description table
- Compilation commands
- UT run commands + expected output
- Coverage section with online report link (GitHub Pages format)
- Manual step-by-step coverage commands
- Final coverage metrics table
- `.gitignore` for coverage artifacts (`*.gcno`, `*.gcda`, `*.gcov` — but keep `coverage_html/` and `coverage.info`)

## File Deliverables

| File | Purpose |
|------|---------|
| `<controller>_interface.json` | Controller interface definition |
| `<controller>_interface.h` | Generated C header (addresses, masks, enums) |
| `<module>_driver.h` | Driver API + hook declarations |
| `<module>_driver.c` | Driver implementation |
| `test_<module>_driver.c` | Unit tests |
| `run_ut.sh` | One-click UT + coverage script |
| `README.md` | Documentation with commands and coverage report link |
| `.gitignore` | Ignore build/coverage intermediates |
| `coverage_html/` | HTML coverage report (committed for online access) |
| `coverage.info` | lcov raw coverage data (committed) |

## Key Lessons Learned

1. **mmap MAP_FIXED is essential** for covering volatile pointer branches in user-space UT without segfault
2. **Probe function must be minimal** — direct calls to default_reg_read/write for all address combinations, no complex loops (loops introduce new uncovered branches)
3. **-O0 is mandatory** — compiler optimization removes/inlines code, breaking line-level coverage
4. **Switch default branch** requires a test case with raw register value outside enum range
5. **Early-return branches** (e.g., "already enabled") need dedicated test cases
6. **RO register write** path must be tested to confirm it's silently ignored
7. **lcov --remove** is more reliable than awk filtering for isolating driver-only coverage
8. **genhtml --branch-coverage** is what makes Line and Branch appear as separate columns in the HTML report
