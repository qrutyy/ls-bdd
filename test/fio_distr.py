import numpy as np
import scipy.stats as stats
import matplotlib.pyplot as plt

RESULTS_FILE = "fio_results.dat"
PLOTS_PATH = "./plots"

# Load data
data = np.loadtxt(RESULTS_FILE, skiprows=1, usecols=(1, 2))
labels = ["Bandwidth", "IOPS"]  # TODO: add latency

# Create individual histograms with descriptions
for i in range(2):
    plt.figure(figsize=(8, 6))
    plt.hist(data[:, i], bins=10, edgecolor='black')
    plt.title(f"Histogram of {labels[i]}")
    plt.xlabel(labels[i])
    plt.ylabel("Frequency")
    
    # Adding a description below the plot
    plt.figtext(0.5, 0.01, f"Plot {i}: {labels[i]} distribution.",
                ha="center", va="center", fontsize=10, wrap=True)
    
    plt.tight_layout()
    plt.savefig(f"{PLOTS_PATH}/{labels[i].replace(' ', '_').lower()}_histogram.png")
    plt.close()

# Check normality for both metrics
for i in range(2):
    stat1, p1 = stats.normaltest(data[:, i])
    stat2, p2 = stats.shapiro(data[:, i])
    print(f"{labels[i]} Normality Test Results: normaltest p-value={p1:.5f}, shapiro p-value={p2:.5f}")
    
    if p1 > 0.05 or p2 > 0.05:
        print(f"✅ {labels[i]} is likely normally distributed.\n")
    else:
        print(f"❌ {labels[i]} is NOT normally distributed. Check for issues.\n")

# Compute statistics (mean, std, confidence intervals)
means = np.mean(data, axis=0)
stds = np.std(data, axis=0, ddof=1)

# Compute 95% confidence intervals
conf_intervals = stats.t.ppf(0.975, df=len(data) - 1) * stats.sem(data, axis=0)

# Rounding function to match error precision
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
    for i in range(2):
        f.write(f"{labels[i]}: {rounded_means[i]} += {rounded_cis[i]}\n")

print("Analysis complete. Histograms and statistics saved.")

