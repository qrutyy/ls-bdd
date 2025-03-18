import numpy as np
import pandas as pd
import scipy.stats as stats
import matplotlib.pyplot as plt
import os

RESULTS_FILE = "logs/fio_results.dat"
PLOTS_PATH = "./plots/histograms"


df = pd.read_csv(RESULTS_FILE, sep=r"\s+", skiprows=0, names=["RunID", "WBS", "RBS", "BW", "IOPS", "SLAT", "CLAT", "LAT", "MODE"])

def clean_numeric(series):
    return pd.to_numeric(series, errors='coerce')

df["BW"] = clean_numeric(df["BW"])
df["IOPS"] = clean_numeric(df["IOPS"])

columns = ["BW", "IOPS"]

os.makedirs(PLOTS_PATH, exist_ok=True)

# Process each (WBS, RBS) combination separately
for wbs in df["WBS"].unique():
    for rbs in df["RBS"].unique():
        subset = df[(df["WBS"] == wbs) & (df["RBS"] == rbs)]

        if subset.empty:
            print(f"Skipping empty dataset for WBS={wbs}, RBS={rbs}")
            continue

        block_plot_path = f"{PLOTS_PATH}"
        os.makedirs(block_plot_path, exist_ok=True)

        for i, label in enumerate(columns):
            plt.figure(figsize=(8, 6))
            plt.hist(subset[label].dropna(), bins=10, edgecolor='black')
            if rbs == 0: 
                base_title = f"(WBS={wbs})"
            else: 
                base_title = f"(RBS={rbs} after WBS={wbs})"

            if label == "BW":
                plt.xlabel("Bandwidth (MB/s)")
                plt.title(f"Histogram of write operations throughput (bw) {base_title}\n")
            else:
                plt.ylabel("IOPS (k/s)")
                plt.title(f"Histogram of write operations throughput (iops) {base_title}\n")

            plt.ylabel("Frequency")
            plt.tight_layout()
            
            if (rbs == 0): 
                plt.savefig(f"{block_plot_path}/write/{label.lower()}_W{wbs}_R{rbs}_histogram.png")
            else:
                plt.savefig(f"{block_plot_path}/read/{label.lower()}_W{wbs}_R{rbs}_histogram.png")

            plt.close()

        # Normality Tests
        print(f"\nWBS={wbs}, RBS={rbs} - Normality Tests:")
        with open(f"{block_plot_path}/stats_summary.txt", "w") as f:
            for label in columns:
                data_values = subset[label].dropna().values

                if len(data_values) < 8:
                    print(f"Skipping normality test for {label} (not enough data)")
                    continue

                stat1, p1 = stats.normaltest(data_values)
                stat2, p2 = stats.shapiro(data_values)
                normality_result = f"{label} Normality Test: normaltest p={p1:.5f}, shapiro p={p2:.5f}"
                
                print(normality_result)
                f.write(normality_result + "\n")

                if p1 > 0.05 or p2 > 0.05:
                    f.write(f"{label} is likely normally distributed.\n\n")
                else:
                    f.write(f"{label} is NOT normally distributed.\n\n")

            # Compute statistics (mean, std, confidence intervals)
            means = subset[columns].mean()
            stds = subset[columns].std(ddof=1)

            # Compute 95% confidence intervals
            conf_intervals = stats.t.ppf(0.975, df=len(subset) - 1) * stats.sem(subset[columns], axis=0)

            def round_to_error(value, error):
                if error == 0:
                    return round(value)
                magnitude = -int(np.floor(np.log10(error)))  
                rounded_error = round(error, magnitude)
                rounded_value = round(value, magnitude)
                return rounded_value, rounded_error

            rounded_results = [round_to_error(m, c) for m, c in zip(means, conf_intervals)]
            rounded_means, rounded_cis = zip(*rounded_results)

            for i, label in enumerate(columns):
                f.write(f"{label}: {rounded_means[i]} ± {rounded_cis[i]}\n")

print("Analysis complete. Histograms and statistics saved for each (WBS, RBS) combination.")
