# Test System Documentation

The test framework is designed to benchmark, verify, and analyze the performance characteristics of log-structured block devices. It includes automated test orchestration, performance measurement, verification testing, and visualization capabilities.

## Automated Testing Tools

The primary interface is `main.sh`, which orchestrates the entire test system workflow.

### Command Line Options

| Action | Option | Description |
|--------|--------|-------------|
| **Run Test Scripts** | `-a` / `--auto` | Execute the complete automated test suites |
| **Environment Setup** | `-s` / `--setup` | Initialize and configure the experiment environment |
| **Performance Analysis** | `-p` / `--perf` | Create flamegraph visualizations for call-stack analysis |
| **Call Stack Analysis** | `-c` / `--cplots` | Generate performance plots from automated test results |

### Configuration Parameters

| Parameter | Option | Description |
|-----------|--------|-------------|
| **Block Device Selection** | `--bd_name <name>` | Name for module reinitialization during performance testing |
| **I/O Depth** | `--io_depth <number>` | Configure the queue depth for I/O operations |
| **Job Concurrency** | `--jobs_num <number>` | Set the number of parallel jobs for testing |

### Usage Examples

```bash
# Complete automated test with custom parameters
./main.sh --auto --bd_name nullb0 --io_depth 32 --jobs_num 4

# Setup environment and run performance analysis
./main.sh --setup --bd_name sda

# Generate flamegraphs for performance profiling
./main.sh --perf --io_depth 16 --jobs_num 8

# Performance testing and plots generation
./main.sh --cplots --setup --bd_name nullb0 --io_depth 32 --jobs_num 8

```

## Manual Testing Tools

### FIO Workflow Directory

The `test/fio/` directory contains specialized FIO configurations for targeted testing:

- **Verification Workflows**: Data integrity and correctness validation
- **Performance Measurement**: Throughput, IOPS, and latency benchmarking

Workflows are presented in two ways - FIO file and .sh script

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
Data files save the extracted data from the test-suite for future dataframe processing and plot generation.

| File | Content |
|------|---------|
| `fio_results.dat` | Primary FIO performance metrics during evaluation |
| `fio_lat_results.dat` | Detailed latency statistics and percentile data |

## Configuration and Customization

### Performance Testing Configuration (`plots.sh`)

The `plots.sh` script provides extensive customization options:

#### Test Types
- **Throughput Testing**: Measure sustained data transfer rates
- **IOPS Testing**: Evaluate I/O operations per second
- **Latency Testing**: Analyze response time characteristics

#### Concurrency Analysis
- **Parameter Sweep**: Test different combinations of `iodepth` and `numjobs`
- **Comparison Mode**: Direct block device vs. `lsvbd1` performance comparison

#### Workload Parameters
- **I/O Mix Ratios**: Configure read/write percentages
- **Block Size Variations**: Test different transfer sizes
- **Access Patterns**: Sequential vs. random I/O patterns
- **Workflow time**: Can be changed for simulating different cases.

### Workflow Templates (`Makefile`)

The `Makefile` contains customizable workflow templates for:
- Standard performance benchmarks
- Verification test procedures
- Custom test parameter combinations

*BTW some performance workflows provide detatailed auxiliary logs with dataframe analysis (normality test, distribution and others).*

