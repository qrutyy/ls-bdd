import numpy as np
import pandas as pd
import scipy.stats as stats
import matplotlib.pyplot as plt

RESULTS_FILE = "logs/fio_results.dat"
PLOTS_PATH = "./plots/histograms"

df = pd.read_csv(RESULTS_FILE, sep="\s+", skiprows=1, names=["RunID", "BS", "BW", "IOPS", "SLAT", "CLAT", "LAT", "MODE"])

def clean_numeric(series):
    return pd.to_numeric(series, errors='coerce')

df["BW"] = clean_numeric(df["BW"])
df["IOPS"] = clean_numeric(df["IOPS"])
df["LAT"] = clean_numeric(df["LAT"])
df["SLAT"] = clean_numeric(df["SLAT"])
df["CLAT"] = clean_numeric(df["CLAT"])

columns = ["BW", "IOPS", "SLAT", "CLAT", "LAT"]
data = df[columns]

for i, label in enumerate(columns):
    plt.figure(figsize=(8, 6))
    plt.hist(data.values[:, i], bins=10, edgecolor='black')
    plt.title(f"Histogram of {label}")
    plt.xlabel(label)
    plt.ylabel("Frequency")
    
    plt.figtext(0.5, 0.01, f"Plot {i}: {label} distribution.",
                ha="center", va="center", fontsize=10, wrap=True)
    
    plt.tight_layout()
    plt.savefig(f"{PLOTS_PATH}/{label.lower()}_histogram.png")
    plt.close()

# Check normality for all metrics
for i, label in enumerate(columns):
    stat1, p1 = stats.normaltest(data.values[:, i])
    stat2, p2 = stats.shapiro(data.values[:, i])
    print(f"\n\n{label} Normality Test Results: normaltest p-value={p1:.5f}, shapiro p-value={p2:.5f}")
    
    if p1 > 0.05 or p2 > 0.05:
        print(f"{label} is likely normally distributed.\n")
    else:
        print(f"{label} is NOT normally distributed.\n")

# Compute statistics (mean, std, confidence intervals)
means = np.mean(data, axis=0)
stds = np.std(data, axis=0, ddof=1)

# Compute 95% confidence intervals
conf_intervals = stats.t.ppf(0.975, df=len(data) - 1) * stats.sem(data, axis=0)

def round_to_error(value, error):
    if error == 0:
        return round(value)
    magnitude = -int(np.floor(np.log10(error)))  
    rounded_error = round(error, magnitude)
    rounded_value = round(value, magnitude)
    return rounded_value, rounded_error

rounded_results = [round_to_error(m, c) for m, c in zip(means, conf_intervals)]
rounded_means, rounded_cis = zip(*rounded_results)

with open(f"{PLOTS_PATH}/stats_summary.txt", "w") as f:
    for i, label in enumerate(columns):
        f.write(f"{label}: {rounded_means[i]} ± {rounded_cis[i]}\n")

print("Analysis complete. Histograms and statistics saved.")

