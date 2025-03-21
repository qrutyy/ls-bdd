import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os
import argparse

parser = argparse.ArgumentParser(description="Generate average plots from fio results.")
parser.add_argument("--raw", action="store_true", help="Save plots to the 'raw' directory")
args = parser.parse_args()

RESULTS_FILE = "logs/fio_results.dat"
PLOTS_PATH = "./plots/avg/raw" if args.raw else "./plots/avg"

colors = ['green', 'red', 'blue', 'brown', 'purple']

df = pd.read_csv(RESULTS_FILE, sep=r"\s+", skiprows=0, names=["RunID", "WBS", "RBS", "BW", "IOPS", "SLAT", "CLAT", "LAT", "MODE"])
print(df)

def clean_numeric(series):
    return pd.to_numeric(series, errors='coerce')

df["BW"] = clean_numeric(df["BW"])
df["IOPS"] = clean_numeric(df["IOPS"])

df = df.dropna()

def plot_metric(metric, ylabel, filename):
    i = 0
    plt.figure(figsize=(8, 6))

    for mode in df["MODE"].unique():
        for (wbs, rbs), subset in df.groupby(["WBS", "RBS"]):
            subset = subset[subset["MODE"] == mode].sort_values(by="RunID")
            if not subset.empty:
                plt.plot(subset["RunID"], subset[metric], marker='o', linestyle='-', linewidth=2, color=colors[i % len(colors)], label=f"WBS={wbs}, RBS={rbs}")
            i += 1

        if plt.gca().has_data():
            plt.legend()

        mode_dir = os.path.join(PLOTS_PATH, mode)
        os.makedirs(mode_dir, exist_ok=True)
        save_path = os.path.join(mode_dir, f"{filename}.png")

        plt.ylabel(ylabel)
        plt.xlabel("Run number")
        plt.title(f"Throughput of {mode} operations ({metric.lower()})\n")
        plt.savefig(save_path)
        plt.clf()
        print(f"Saved: {save_path}")

    plt.close()

plot_metric("BW", "Bandwidth (MB/s)", "bandwidth_plot")
plot_metric("IOPS", "IOPS", "iops_plot")

print("Analysis complete. Graphs saved.")

