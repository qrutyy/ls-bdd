import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os
import argparse

parser = argparse.ArgumentParser(description="Generate average plots from fio results.")
parser.add_argument("--raw", action="store_true", help="Save plots to the 'raw' directory")
parser.add_argument("--tp", action="store_true", help="Run throughput plot generator")
args = parser.parse_args()

RESULTS_FILE = "logs/fio_results.dat"
PLOTS_PATH = "./plots/avg/raw" if args.raw else "./plots/avg/vbd"

DEVICE = "ram0" if args.raw else "lsvbd1"

colors = ['green', 'red', 'blue', 'brown', 'purple']

df = pd.read_csv(RESULTS_FILE, sep=r"\s+", skiprows=0, names=["RunID", "BS", "MIX", "BW", "IOPS", "MODE"])
print(df)

def clean_numeric(series):
    return pd.to_numeric(series, errors='coerce')

df["BW"] = clean_numeric(df["BW"])
df["IOPS"] = clean_numeric(df["IOPS"])

df = df.dropna()

def plot_tp(df):
    i = 0
    plt.figure(figsize=(8, 6))
    df = df[df["MODE"] == "tp"]

    for bs in df["BS"].unique(): 
        for mix in df["MIX"].unique():
            subset = df[(df["BS"] == bs) & (df["MODE"] == "tp") & (df["MIX"] == mix)].sort_values(by="RunID")

            label_f = f"BS={bs}, MIX={mix}"

            if not subset.empty:
                plt.plot(subset["RunID"], subset["BW"], marker='o', linestyle='-', linewidth=2, color=colors[i % len(colors)], label=label_f)
            i += 1

    if plt.gca().has_data():
        plt.legend()

    mode_dir = os.path.join(PLOTS_PATH, "tp")
    os.makedirs(mode_dir, exist_ok=True)
    save_path = os.path.join(mode_dir, f"bandwidth_plot.png")

    plt.ylabel("Bandwidth (MB/s)")
    plt.xlabel("Run number")
    plt.title(f"Throughput of {mix} operations mix with {DEVICE}\n")
    plt.savefig(save_path)
    plt.clf()
    print(f"Saved: {save_path}")

    plt.close()

def plot_iops(df):
    df = df[df["MODE"] == "iops"]

    for bs in df["BS"].unique(): 
        plt.figure(figsize=(8, 6))
        for mix in df["MIX"].unique():
            subset = df[(df["MODE"] == "iops") & (df["BS"] == bs) & (df["MIX"] == mix)].sort_values(by="RunID")
            if subset.empty:
                continue

            label_f = f"BS={bs}, MIX={mix}"

            plt.plot(subset["RunID"], subset["IOPS"], marker='o', linestyle='-', linewidth=2, 
                     color=colors[hash(mix) % len(colors)], label=label_f)

        if plt.gca().has_data():
            plt.legend()

        mode_dir = os.path.join(PLOTS_PATH, "iops")
        os.makedirs(mode_dir, exist_ok=True)
        save_path = os.path.join(mode_dir, f"iops_plot_{bs}BS.png")

        plt.ylabel("IOPS (K/s)")
        plt.xlabel("Run number")
        plt.title(f"Total number of {mix} operations per second (IOPS) with {DEVICE}\n")
        plt.savefig(save_path)
        plt.close()
        print(f"Saved: {save_path}")

if args.tp:
    plot_tp(df)
else:
    plot_iops(df)

print("Analysis complete. Graphs saved.")

