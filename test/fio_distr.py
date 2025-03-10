import numpy as np
import scipy.stats as stats
import matplotlib.pyplot as plt

# Load data
data = np.loadtxt("$RESULTS_FILE", skiprows=1, usecols=(1,2,3))
labels = ["Bandwidth (MB/s)", "IOPS", "Latency (ms)"]

plt.figure(figsize=(10, 4))

# Histogram for each metric
for i in range(3):
    plt.subplot(1, 3, i+1)
    plt.hist(data[:, i], bins=10, edgecolor='black')
    plt.title(labels[i])

plt.savefig("$PLOTS_PATH/histograms.png")
plt.close()

# Check normality
for i in range(3):
    stat1, p1 = stats.normaltest(data[:, i])
    stat2, p2 = stats.shapiro(data[:, i])
    print(f"{labels[i]} Normality Test Results: normaltest p-value={p1:.5f}, shapiro p-value={p2:.5f}")
    
    if p1 > 0.05 or p2 > 0.05:
        print(f"✅ {labels[i]} is likely normally distributed.")
    else:
        print(f"❌ {labels[i]} is NOT normally distributed. Check for issues.")

# Compute statistics
means = np.mean(data, axis=0)
stds = np.std(data, axis=0, ddof=1)

# Compute 95% confidence intervals
conf_intervals = stats.t.ppf(0.975, df=len(data) - 1) * stats.sem(data, axis=0)

# Rounding rules
def round_to_error(value, error):
    if error == 0:
        return round(value)
    magnitude = -int(np.floor(np.log10(error)))  # Determine precision
    rounded_error = round(error, magnitude)
    rounded_value = round(value, magnitude)
    return rounded_value, rounded_error

rounded_results = [round_to_error(m, c) for m, c in zip(means, conf_intervals)]
rounded_means, rounded_cis = zip(*rounded_results)

# Save results
with open("$PLOTS_PATH/stats_summary.txt", "w") as f:
    for i in range(3):
        f.write(f"{labels[i]}: {rounded_means[i]} += {rounded_cis[i]}\n")

print("Analysis complete. Histograms and statistics saved.")

