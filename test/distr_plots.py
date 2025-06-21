import argparse
import numpy as np
import pandas as pd
import scipy.stats as stats
import matplotlib.pyplot as plt
import os

"""
Simply plots the values distribution of different metrics. The result is a histogram in PLOTS_PATH directory.
Can require some args (depends on your case) - check the parser below.
"""

parser = argparse.ArgumentParser(
    description="Generate distribution plots from fio results."
)
parser.add_argument(
    "--raw", action="store_true", help="Save plots to the 'raw' directory"
)
parser.add_argument("--rewrite", action="store_true", help="Rewrite operation mode")

args = parser.parse_args()

RESULTS_FILE = "logs/fio_results.dat"
PLOTS_PATH = "./plots/distribution/raw" if args.raw else "./plots/distribution/vbd"
PLOTS_PATH += "/rewrite" if args.rewrite else "/non_rewrite"
DEVICE = "nullb0" if args.raw else "lsvbd1"

try:
    df = pd.read_csv(
        RESULTS_FILE,
        sep=r"\s+",
        skiprows=0,
        names=["RunID", "BS", "MIX", "BW", "IOPS", "MODE"],
    )
except FileNotFoundError:
    print(f"Error: Results file not found at {LAT_RESULTS_FILE}")
    exit()
except pd.errors.ParserError as e:
    print(f"Error parsing {LAT_RESULTS_FILE}: {e}")
    print("Please ensure the file format is correct and matches the expected columns.")
    exit()


def clean_numeric(series):
    return pd.to_numeric(series, errors="coerce")


df["BW"] = clean_numeric(df["BW"])
df["IOPS"] = clean_numeric(df["IOPS"])
df = df.dropna()

columns = ["BW", "IOPS"]
os.makedirs(PLOTS_PATH, exist_ok=True)

for mode in df["MODE"].unique():
    mode_dir = os.path.join(PLOTS_PATH, mode)
    os.makedirs(mode_dir, exist_ok=True)

    for bs in df["BS"].unique():
        for mix in df["MIX"].unique():
            subset = df[(df["BS"] == bs) & (df["MIX"] == mix) & (df["MODE"] == mode)]

            if subset.empty:
                print(f"Skipping empty dataset for BS={bs}, MIX={mix}")
                continue

            for label in columns:
                plt.figure(figsize=(8, 6))
                plt.hist(subset[label].dropna(), bins=10, edgecolor="black")

                base_title = f"BS={bs} на {DEVICE}"
                title_ds_status = (
                    " используя список с пропусками\n," if (DEVICE == "lsvbd1") else ""
                )
                title_rewrite_status = (
                    "(с прогревом)" if args.rewrite else "(без прогрева)"
                )
                title_main_label = (
                    "пропускной способности (BW)"
                    if label == "BW"
                    else "количества операций IO в секунду (IOPS)"
                )
                if label == "BW":
                    plt.xlabel("Пропускная способность (ГБ/с)")
                else:
                    plt.xlabel("IOPS (тыс. операций/с)")

                plt.title(
                    f"Гистрограмма {title_main_label}{title_ds_status} при {mix} операциях {base_title} {title_rewrite_status}\n"
                )

                plt.ylabel("Частота")
                plt.tight_layout()

                save_path = os.path.join(
                    mode_dir, f"{mode}_{label.lower()}_BS{bs}_MIX{mix}.png"
                )
                plt.savefig(save_path)
                plt.close()
                print(f"Saved: {save_path}")

            print(f"\n{mode} BS={bs}, MIX={mix} - Normality Tests:")
            stats_file = os.path.join(mode_dir, f"{mode}_stats_summary.txt")

            with open(stats_file, "a") as f:
                for label in columns:
                    data_values = subset[label].dropna().values

                    if len(data_values) < 8:
                        print(f"Skipping normality test for {label} (not enough data)")
                        continue

                    stat1, p1 = stats.normaltest(data_values)
                    stat2, p2 = stats.shapiro(data_values)
                    normality_result = f"{label} Normality Test: normaltest p={p1:.5f}, shapiro p={p2:.5f}"

                    if p1 > 0.05 or p2 > 0.05:
                        normality_result += (
                            f" → {label} is likely normally distributed."
                        )
                    else:
                        normality_result += f" → {label} is NOT normally distributed."

                    f.write(normality_result + "\n")
                    print(normality_result)

print("Analysis complete. Histograms and statistics saved.")
