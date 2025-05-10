import argparse
import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os

parser = argparse.ArgumentParser(description="Generate average plots from fio results.")
parser.add_argument("--raw", action="store_true", help="Save plots to the 'raw' directory")
args = parser.parse_args()

LAT_RESULTS_FILE = "logs/fio_lat_results.dat"
PLOTS_PATH = "./plots/latency/raw" if args.raw else "./plots/latency/vbd"

df = pd.read_csv(LAT_RESULTS_FILE, sep=r"\s+", skiprows=0, names=[
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
        plt.title(f"{ylabel} (Block Size {bs})")
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
    plt.title(f"{ylabel} (All Block Sizes)")
    plt.legend(title="RW Mix (BS)")
    plt.grid()

    if not os.path.exists(PLOTS_PATH):
        os.makedirs(PLOTS_PATH)

    save_path = os.path.join(PLOTS_PATH, f"{filename}.png")
    plt.savefig(save_path)
    plt.close()
    print(f"Saved united plot: {save_path}")

def plot_boxplot_latency():
    """Generate boxplot for all the latencies (with and without the outliers)"""
    plt.figure(figsize=(8, 6))
     
    metric_cols = ["Avg_LAT", "Max_LAT", "P95_LAT"]
    latencies = [df[col].dropna() for col in metric_cols]
    
    plt.boxplot(latencies, labels=["Avg_LAT", "Max_LAT", "P95_LAT"], showfliers=True, patch_artist=True)
    plt.ylabel("Latency (ns)")
    plt.title("Latency Distribution with Outliers")
    plt.grid(True, which="both", linestyle="--", linewidth=0.5)
    plt.axis([0.5, 3.5, df[["Avg_LAT", "Max_LAT", "P95_LAT"]].min().min(), df[["Avg_LAT", "Max_LAT", "P95_LAT"]].max().max()])
    plt.savefig(f"{PLOTS_PATH}/latency_boxplot_with_outliers.png")
    plt.close()
    plt.clf()

    plt.figure(figsize=(8, 6))
    plt.boxplot(latencies, labels=["Avg_LAT", "Max_LAT", "P95_LAT"], showfliers=False, patch_artist=True)
    plt.ylabel("Latency (ns)")
    plt.title("Latency Distribution without Outliers")
    plt.grid(True, which="both", linestyle="--", linewidth=0.5)
    plt.axis([0.5, 3.5, df[["Avg_LAT", "Max_LAT", "P95_LAT"]].min().min(), df[["Avg_LAT", "Max_LAT", "P95_LAT"]].max().max()])
    plt.savefig(f"{PLOTS_PATH}/latency_boxplot_without_outliers.png")
    plt.close()
    plt.clf()
    
    print(f"Saved boxplots: {PLOTS_PATH}/latency_boxplot_without_outliers {PLOTS_PATH}/latency_boxplot_with_outliers")


# Generate separate latency plots per block size
plot_metric_by_bs("Avg_LAT", "Average Latency (ns)", "avg_latency")
plot_metric_by_bs("Max_LAT", "Max Latency (ns)", "max_latency")
plot_metric_by_bs("P95_LAT", "95th Percentile Latency (ns)", "p95_latency")

# Generate united latency plots (all block sizes together)
plot_united_metric("Avg_LAT", "Average Latency (ns)", "avg_latency_united")
plot_united_metric("Max_LAT", "Max Latency (ns)", "max_latency_united")
plot_united_metric("P95_LAT", "95th Percentile Latency (ns)", "p95_latency_united") 

#same for slat
plot_metric_by_bs("Avg_SLAT", "Average Submission Latency (ns)", "avg_slatency")
plot_metric_by_bs("Max_SLAT", "Max Submission Latency (ns)", "max_slatency")
plot_metric_by_bs("P95_SLAT", "95th Percentile Submission Latency (ns)", "p95_slatency")

plot_united_metric("Avg_SLAT", "Average Submission Latency (ns)", "avg_slatency_united")
plot_united_metric("Max_SLAT", "Max Submission Latency (ns)", "max_slatency_united")
plot_united_metric("P95_SLAT", "95th Percentile Submission Latency (ns)", "p95_slatency_united")

plot_boxplot_latency()

print("Latency analysis complete. Graphs saved.")

