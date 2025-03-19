import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os

LAT_RESULTS_FILE = "logs/fio_lat_results.dat"
PLOTS_PATH = "./plots/latency"

df = pd.read_csv(LAT_RESULTS_FILE, sep="\s+", skiprows=0, names=[
    "RunID", "BS", "Avg_SLAT", "Avg_CLAT", "Avg_LAT",
    "Max_SLAT", "Max_CLAT", "Max_LAT",
    "P95_SLAT", "P95_CLAT", "P95_LAT", "RW_MIX"
])

def clean_numeric(series):
    return pd.to_numeric(series, errors='coerce')

for col in df.columns[2:-1]:  # Skip RunID, BS, and RW_MIX
    df[col] = clean_numeric(df[col])

df = df.dropna()

def plot_metric_by_bs(metric, ylabel, filename_prefix):
    """Generate separate plots for each block size"""
    unique_bs = df["BS"].unique()
    
    for bs in unique_bs:
        plt.figure(figsize=(8, 6))
        subset = df[df["BS"] == bs]

        for rw_mix in subset["RW_MIX"].unique():
            rw_subset = subset[subset["RW_MIX"] == rw_mix].sort_values(by="RunID")
            if not rw_subset.empty:
                plt.plot(rw_subset["RunID"], rw_subset[metric], marker='o', linestyle='-', label=f"{rw_mix}")

        plt.xlabel("Run number")
        plt.ylabel(ylabel)
        plt.title(f"{ylabel} vs. Run (Block Size {bs})")
        plt.legend(title="RW Mix")
        plt.grid()

        if not os.path.exists(PLOTS_PATH):
            os.makedirs(PLOTS_PATH)

        save_path = os.path.join(PLOTS_PATH, f"{filename_prefix}_bs_{bs}.png")
        plt.savefig(save_path)
        plt.close()
        print(f"Saved plot: {save_path}")


def plot_united_metric(metric, ylabel, filename):
    """Generate a single plot combining all block sizes"""
    plt.figure(figsize=(8, 6))

    for bs in df["BS"].unique():
        subset = df[df["BS"] == bs]
        for rw_mix in subset["RW_MIX"].unique():
            rw_subset = subset[subset["RW_MIX"] == rw_mix].sort_values(by="RunID")
            if not rw_subset.empty:
                plt.plot(rw_subset["RunID"], rw_subset[metric], marker='o', linestyle='-', label=f"{rw_mix} ({bs})")

    plt.xlabel("Test Run")
    plt.ylabel(ylabel)
    plt.title(f"{ylabel} vs. Run (All Block Sizes)")
    plt.legend(title="RW Mix (BS)")
    plt.grid()

    if not os.path.exists(PLOTS_PATH):
        os.makedirs(PLOTS_PATH)

    save_path = os.path.join(PLOTS_PATH, f"{filename}.png")
    plt.savefig(save_path)
    plt.close()
    print(f"Saved united plot: {save_path}")

# Generate separate latency plots per block size
plot_metric_by_bs("Avg_LAT", "Average Latency (ms)", "avg_latency")
plot_metric_by_bs("Max_LAT", "Max Latency (ms)", "max_latency")
plot_metric_by_bs("P95_LAT", "95th Percentile Latency (ms)", "p95_latency")

# Generate united latency plots (all block sizes together)
plot_united_metric("Avg_LAT", "Average Latency (ms)", "avg_latency_united")
plot_united_metric("Max_LAT", "Max Latency (ms)", "max_latency_united")
plot_united_metric("P95_LAT", "95th Percentile Latency (ms)", "p95_latency_united")

print("Latency analysis complete. Graphs saved.")

