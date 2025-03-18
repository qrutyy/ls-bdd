import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os

RESULTS_FILE = "logs/fio_results.dat"
PLOTS_PATH = "./plots/avg"

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
            subset_mean = [np.mean(subset[metric])] * len(subset["RunID"])
            print(subset_mean)
            if not subset.empty: 
                plt.plot(subset["RunID"], subset[metric], marker='o', linestyle='-', linewidth=2, color=colors[i], label=f"{mode.upper()} ({wbs}, {rbs})")
                plt.plot(subset["RunID"], subset_mean, linestyle='--', linewidth=2, color=colors[i], label=f"AVG: {mode.upper()} ({wbs}, {rbs})") 
                i+=1

        if plt.gca().has_data():
            plt.legend()

        mode_dir = os.path.join(PLOTS_PATH, mode)
        os.makedirs(mode_dir, exist_ok=True)

        save_path = os.path.join(mode_dir, f"{filename}.png")
        
        if metric == "BW":
            plt.ylabel("Bandwidth (MB/s)")
            plt.title(f"Throughput of write operations (bw)\n")
        else:
            plt.ylabel("IOPS (k/s)")
            plt.title(f"Throughput of write operations (iops)\n")

        plt.xlabel("Run number")

        plt.savefig(save_path)
        print(f"Saved: {save_path}")

    plt.close()

plot_metric("BW", "Bandwidth (MB/s)", "bandwidth_plot")
plot_metric("IOPS", "IOPS", "iops_plot")

print("Analysis complete. Graphs saved.")
