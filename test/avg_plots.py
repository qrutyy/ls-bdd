import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os

RESULTS_FILE = "logs/fio_results.dat"
PLOTS_PATH = "./plots/avg"

df = pd.read_csv(RESULTS_FILE, sep="\s+", skiprows=0, names=["RunID", "BS", "BW", "IOPS", "SLAT", "CLAT", "LAT", "MODE"])
print(df)

def clean_numeric(series):
    return pd.to_numeric(series, errors='coerce')

df["BW"] = clean_numeric(df["BW"])
df["IOPS"] = clean_numeric(df["IOPS"])
df["LAT"] = clean_numeric(df["LAT"])
df["SLAT"] = clean_numeric(df["SLAT"])
df["CLAT"] = clean_numeric(df["CLAT"])

df = df.dropna()

def plot_metric(metric, ylabel, filename):
    plt.figure(figsize=(8, 6))

    for mode in df["MODE"].unique():
        for bs in df["BS"].unique():
            subset = df[(df["BS"] == bs) & (df["MODE"] == mode)]
            subset = subset.sort_values(by="RunID")
            if not subset.empty: 
                plt.plot(subset["RunID"], subset[metric], marker='o', linestyle='-', label=f"{mode.upper()} {bs}")
            else:
                print(f"Plotting {metric} for BS={bs}, Mode={mode}, RunID={subset['RunID'].head()}")


    plt.xlabel("Test Run") 
    plt.ylabel(ylabel) 
    plt.title(f"{ylabel} vs. Run") 
    plt.legend()
    plt.grid()  

    if not os.path.exists(PLOTS_PATH):
        os.makedirs(PLOTS_PATH)
    
    plt.savefig(f"{PLOTS_PATH}/{filename}.png")  
    plt.close()

plot_metric("BW", "Bandwidth (MB/s)", "bandwidth_plot")
plot_metric("IOPS", "IOPS", "iops_plot")
plot_metric("SLAT", "Submission Latency (ms)", "slatency_plot")
plot_metric("CLAT", "Completion Latency (ms)", "clatency_plot")
plot_metric("LAT", "Latency (ms)", "latency_plot")

print("Analysis complete. Graphs saved.")

