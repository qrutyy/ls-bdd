# Test System Documentation

The test framework is designed to benchmark, verify, and analyze the performance characteristics of log-structured block devices. It includes automated test orchestration, performance measurement, verification testing, and visualization capabilities.

**Limitations:**
* The performance measurement system does not support mixed workflows.

## Automated Testing Tools

The primary interface is `main.sh`, which orchestrates the entire test system workflow.

### Command Line Options

| Action                   | Option            | Description                                           |
| ------------------------ | ----------------- | ----------------------------------------------------- |
| **Run Test Scripts**     | `-a` / `--auto`   | Execute the complete automated test suite             |
| **Environment Setup**    | `-s` / `--setup`  | Initialize and configure the experimental environment |
| **Performance Analysis** | `-p` / `--perf`   | Generate flamegraphs for call stack analysis          |
| **Call Stack Analysis**  | `-c` / `--cplots` | Produce performance plots from automated test results |

### Configuration Parameters

Configuration parameters are defined in `configurable_params.sh`.
Edit this file as needed, then run `main.sh` with the desired mode.
For example:

```bash
./main.sh --setup --cplots
```

Plot titles can also be modified in `configurable_params.sh`.

The `plots.sh` script provides extensive customization options:

- **Test Types**
  * **IOPS Testing**: Evaluates input/output operations per second
  * **Latency Testing**: Analyzes response time characteristics

- **Concurrency Analysis**
  * **Parameter Sweep**: Tests various combinations of `iodepth` and `numjobs`
  * **Comparison Mode**: Compares raw block device performance with `lsvbd1`

- **Workload Parameters**
  * **I/O Workflow**: Configures read/write operation types
  * **Block Size Variations**: Tests different transfer sizes
  * **Access Patterns**: Sequential vs. random I/O workloads
  * **Workflow Duration**: Adjustable to simulate different scenarios


### Plots examples
| ![Figure 1](https://github.com/qrutyy/ls-bdd/blob/main/test/plots/iops/IOPS_general_hist_nj8_id32.png) | ![Figure 2](https://github.com/qrutyy/ls-bdd/blob/main/test/plots/iops/AVG_IOPS_bars_idnj_0-100_8_randrw.png) |
|----------------------------|----------------------------|
| *Figure 1: AVG IOPS for different operations with specified NJ/ID, BS*    | *Figure 2: AVG IOPS for different NJ/ID with specified BS*     |

| ![Figure 3](https://github.com/qrutyy/ls-bdd/blob/main/test/plots/lat/P99_LAT_general_hist_nj8_id32.png) |
|----------------------------|
| *Figure 3: P99 LAT for different operations with specified NJ/ID, BS*    |


## Manual Testing Tools

### FIO Workflow Directory

The `test/fio/` directory contains specialized FIO configurations for targeted testing:

* **Verification Workflows**: Validate data integrity and correctness
* **Performance Measurement**: Benchmark throughput, IOPS, and latency

Each workflow is provided in two forms: an FIO configuration file and a corresponding `.sh` script.

## Output and Results

### Directory Structure

```
test/
├── plots/          # Generated performance visualizations
├── logs/           # Test execution logs and debug output
├── fio/            # Manual FIO test configurations
└── manualsrc/      # Text files for manual testing
```

### Performance Data Files

Performance data files store extracted results from automated test suites for later dataframe processing and plot generation.

| File                  | Content                                                     |
| --------------------- | ----------------------------------------------------------- |
| `fio_results.dat`     | Primary FIO performance metrics collected during evaluation |
| `fio_lat_results.dat` | Detailed latency statistics and percentile data             |

### Workflow Templates (`Makefile`)

The `Makefile` provides customizable workflow templates for:

* Standard performance benchmarks
* Verification test procedures
* Custom test parameter combinations
