import argparse
import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os
import re

"""
Plots latency behaviour in different cases. Plots in form of box plot and histograms.

Can be used in 2 modes:
 - simple mode - just plots the boxplot with general AVG, P99 and MAX latencies.
 - conc_mode (@arg) - is used for concurrency performance evaluation.
   Requires NUMJOBS and IODEPTH columns in imported DataFrame.
   For each RW_MIX and BS plots a histogram with every NUMJOBS-IODEPTH mix.

Can require some args (depends on your case) - check the parser below.
"""
parser = argparse.ArgumentParser(description="Generate average plots from fio results.")
parser.add_argument(
    "--raw", action="store_true", help="Save plots to the 'raw' directory"
)
parser.add_argument(
    "--rewrite",
    action="store_true",
    help="Rewrite operation mode (e.g. 'with warm up')",
)
parser.add_argument(
    "--conc_mode",
    action="store_true",
    help="Generate histograms for concurrent mode (IODEPTH, NUMJOBS)",
)

args = parser.parse_args()

LAT_RESULTS_FILE = "logs/fio_lat_results.dat"
DEVICE = "nullb0" if args.raw else "lsvbd1"

base_plot_dir = "./plots/latency/raw" if args.raw else "./plots/latency/vbd"
base_plot_dir += "/rewrite" if args.rewrite else "/non_rewrite"

if args.conc_mode:
    PLOTS_PATH = os.path.join(base_plot_dir, "conc_histograms")
else:
    PLOTS_PATH = base_plot_dir

# Define column names, including IODEPTH and NUMJOBS for conc_mode
# If these are not always present, you might need more sophisticated loading or error handling
column_names = [
    "RunID",
    "BS",
    "Avg_SLAT",
    "Avg_CLAT",
    "Avg_LAT",
    "Max_SLAT",
    "Max_CLAT",
    "Max_LAT",
    "P99_SLAT",
    "P99_CLAT",
    "P99_LAT",
    "RW_MIX",
]
if args.conc_mode:
    column_names.extend(["IODEPTH", "NUMJOBS"])


def parse_block_size_for_sorting(bs_str):
    """
    Parses a block size string and converts it to bytes for sorting.
    @param bs_str: The block size string. @type bs_str: str
    @return: The block size in bytes. @rtype: int
    """
    bs_str_lower = str(bs_str).lower()
    match = re.match(r"(\d+)([kmgtpbs]*)", bs_str_lower)
    if not match:
        return 0
    num_part = int(match.group(1))
    unit_part = match.group(2)
    return num_part * 1024


def plot_metric_by_bs(metric, ylabel, filename_prefix):
    """
    Generate separate plots for each block size.

    @param metric: Metric column name (f.e. "P99_SLAT", "Avg_LAT", ...)
    @param ylabel: Y axis label ;)
    @param filename_prefix: Filename prefix that depends on the metric (f.e. "p99_latency")
    """
    unique_bs = sorted(list(df["BS"].unique()), key=parse_block_size_for_sorting)

    for bs in unique_bs:
        plt.figure(figsize=(10, 6))
        subset = df[df["BS"] == bs]

        for rw_mix in subset["RW_MIX"].unique():
            rw_subset = subset[subset["RW_MIX"] == rw_mix].sort_values(by="RunID")
            if not rw_subset.empty:
                plt.plot(
                    rw_subset["RunID"],
                    rw_subset[metric],
                    marker="o",
                    linestyle="-",
                    label=f"{rw_mix}",
                )

        plt.xlabel("Номер итерации")
        plt.ylabel(ylabel)
        title_status = "(с прогревом)" if args.rewrite else "(без прогрева)"
        title_ds_status = (
            " используя список с пропусками,\n" if (DEVICE == "lsvbd1") else ""
        )
        plt.title(f"{ylabel}{title_ds_status} BS={bs} на {DEVICE}, {title_status}")
        plt.legend(title="Соотношение операций чтения к записи")
        plt.grid(True, linestyle="--", alpha=0.7)
        plt.tight_layout()

        if not os.path.exists(PLOTS_PATH):
            os.makedirs(PLOTS_PATH)

        save_path = os.path.join(PLOTS_PATH, f"{filename_prefix}_bs_{bs}.png")
        plt.savefig(save_path)
        plt.close()
        print(f"Saved plot: {save_path}")


def plot_united_metric(metric, ylabel, filename):
    """
    Generate a single plot combining all block sizes

    @param metric: Metric column name (f.e. "P99_SLAT", "Avg_LAT", ...)
    @param ylabel: Y axis label ;)
    @param filename_prefix: Filename prefix that depends on the metric (f.e. "p99_latency_united")
    """

    plt.figure(figsize=(12, 7))
    unique_bs_sorted = sorted(list(df["BS"].unique()), key=parse_block_size_for_sorting)

    for bs in unique_bs_sorted:
        subset = df[df["BS"] == bs]
        for rw_mix in subset["RW_MIX"].unique():
            rw_subset = subset[subset["RW_MIX"] == rw_mix].sort_values(by="RunID")
            if not rw_subset.empty:
                plt.plot(
                    rw_subset["RunID"],
                    rw_subset[metric],
                    marker="o",
                    linestyle="-",
                    label=f"{rw_mix} ({bs})",
                )

    plt.xlabel("Номер итерации")
    plt.ylabel(ylabel)
    title_status = "(с прогревом)" if args.rewrite else "(без прогрева)"
    title_ds_status = (
        " используя список с пропусками,\n" if (DEVICE == "lsvbd1") else ""
    )
    plt.title(f"{ylabel}{title_ds_status} на {DEVICE} {title_status}")
    plt.legend(title="Соотношение операций чтения к записи, (BS)", loc="best")
    plt.grid(True, linestyle="--", alpha=0.7)
    plt.tight_layout()

    if not os.path.exists(PLOTS_PATH):
        os.makedirs(PLOTS_PATH)

    save_path = os.path.join(PLOTS_PATH, f"{filename}.png")
    plt.savefig(save_path)
    plt.close()
    print(f"Saved united plot: {save_path}")


def plot_boxplot_latency():
    """Generate boxplot for all the latencies (with and without the outliers)"""
    metric_cols = ["Avg_LAT", "Max_LAT", "P99_LAT"]
    latencies = [
        df[col].dropna()
        for col in metric_cols
        if col in df.columns and not df[col].dropna().empty
    ]

    if not latencies or len(latencies) != len(metric_cols):
        print("Warning: Not enough data for boxplots. Skipping.")
        return

    tick_labels_actual = [
        col for col in metric_cols if col in df.columns and not df[col].dropna().empty
    ]

    for show_outliers in [True, False]:
        plt.figure(figsize=(8, 6))
        bp = plt.boxplot(
            latencies,
            tick_labels=tick_labels_actual,
            showfliers=show_outliers,
            patch_artist=True,
        )
        colors = ["lightblue", "lightgreen", "lightpink"]
        for patch, color in zip(bp["boxes"], colors):
            patch.set_facecolor(color)

        plt.ylabel("Задержка (мс)")
        title_status = "(с прогревом," if args.rewrite else "(без прогрева,"
        outlier_status = " с выбросами)" if show_outliers else " без выбросов)"
        title_ds_status = (
            " используя список с пропусками,\n" if (DEVICE == "lsvbd1") else ""
        )
        plt.title(
            f"Распределение задержки{title_ds_status}{outlier_status}, на {DEVICE} {title_status}"
        )
        plt.grid(True, which="both", linestyle="--", linewidth=0.5)
        plt.tight_layout()

        if not os.path.exists(PLOTS_PATH):
            os.makedirs(PLOTS_PATH)

        filename_suffix = "with_outliers" if show_outliers else "without_outliers"
        save_path = os.path.join(PLOTS_PATH, f"latency_boxplot_{filename_suffix}.png")
        plt.savefig(save_path)
        plt.close()
        print(f"Saved boxplot: {save_path}")

    print(f"Saved boxplots to directory: {PLOTS_PATH}")


def plot_latency_histograms_conc_mode(df_conc, bs_val, mix_val, is_rewrite):
    """
    Generates grouped bar chart for Avg_LAT, Avg_SLAT, Avg_CLAT vs. (IODEPTH, NUMJOBS)
    for a specific BS and MIX in concurrent mode.

    @param df_conc: Subset of general DataFrame
    @param bs_val: Unique block sizes
    @param mix_val: Current R/W mix
    @param is_rewrite: Rewrite mode flag for title
    """
    if df_conc.empty:
        print(
            f"No data for BS={bs_val}, MIX={mix_val} for concurrent latency histograms."
        )
        return

    df_plot_data = df_conc.copy()
    df_plot_data["IODEPTH"] = df_plot_data["IODEPTH"].astype(int)
    df_plot_data["NUMJOBS"] = df_plot_data["NUMJOBS"].astype(int)

    # Group by IODEPTH and NUMJOBS, calculate mean for latency metrics
    avg_data = df_plot_data.groupby(["IODEPTH", "NUMJOBS"], as_index=False)[
        ["Avg_LAT", "Avg_SLAT", "Avg_CLAT"]
    ].mean()

    if avg_data.empty:
        print(
            f"No data after grouping for BS={bs_val}, MIX={mix_val} for concurrent histograms."
        )
        return

    # Sort for consistent plotting order
    sorted_avg_data = avg_data.sort_values(by=["IODEPTH", "NUMJOBS"])
    if sorted_avg_data.empty:
        return

    x_labels = [
        f"ID={int(row['IODEPTH'])}, NJ={int(row['NUMJOBS'])}"
        for _, row in sorted_avg_data.iterrows()
    ]
    num_configs = len(x_labels)

    metrics_to_plot = ["Avg_SLAT", "Avg_CLAT", "Avg_LAT"]
    metric_display_names = ["Avg Submit Lat.", "Avg Completion Lat.", "Avg Total Lat."]
    colors = ["#1f77b4", "#ff7f0e", "#2ca02c"]  # Blue, Orange, Green

    x = np.arange(num_configs)
    width = 0.25
    num_metrics = len(metrics_to_plot)

    fig, ax = plt.subplots(figsize=(max(10, num_configs * 1.5), 7.5))

    for i, metric in enumerate(metrics_to_plot):
        y_values = sorted_avg_data[metric].tolist()
        # Calculate offset for each group of bars
        # The middle bar will be centered at x, others offset relative to it.
        offset = width * (i - (num_metrics - 1) / 2.0)
        rects = ax.bar(
            x + offset,
            y_values,
            width,
            label=metric_display_names[i],
            color=colors[i],
            edgecolor="black",
            zorder=2,
        )

        for rect in rects:
            height = rect.get_height()
            ax.annotate(
                f"{height:.0f}",
                xy=(rect.get_x() + rect.get_width() / 2, height),
                xytext=(0, 3),  # 3 points vertical offset
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=7,
            )

    ax.set_ylabel("Средняя задержка (мс)", fontsize=12, labelpad=10)
    ax.set_xlabel("Конфигурация (IODEPTH & NUMJOBS)", fontsize=12, labelpad=15)
    title_status = "(с прогревом)" if is_rewrite else "(без прогрева)"
    title_ds_status = (
        " используя список с пропусками,\n" if (DEVICE == "lsvbd1") else ""
    )
    ax.set_title(
        f"Средние значения задержки в зависимости от ID/NJ{title_ds_status} при BS={bs_val}, MIX={mix_val} на {DEVICE} {title_status}",
        fontsize=13,
        pad=20,
    )
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels, rotation=45, ha="right", fontsize=9)
    ax.legend(title="Виды метрик задержки")
    ax.grid(axis="y", linestyle="--", alpha=0.7, zorder=1)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    fig.tight_layout()

    if not os.path.exists(PLOTS_PATH):
        os.makedirs(PLOTS_PATH)

    safe_bs_val = str(bs_val).replace("/", "_")
    safe_mix_val = str(mix_val).replace("/", "_")
    filename = f"latency_hist_conc_{safe_mix_val}_{safe_bs_val}.png"
    save_path = os.path.join(PLOTS_PATH, filename)

    try:
        plt.savefig(save_path)
        print(f"Saved concurrent histogram: {save_path}")
    except Exception as e:
        print(f"Error saving plot {save_path}: {e}")
    finally:
        plt.close(fig)


try:
    df = pd.read_csv(
        LAT_RESULTS_FILE,
        sep=r"\s+",
        skiprows=0,
        names=column_names,
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


print(df)

# Clean numeric columns: Skip RunID, BS, RW_MIX.
cols_to_clean = [col for col in df.columns if col not in ["RunID", "BS", "RW_MIX"]]
for col in cols_to_clean:
    if col in df.columns:
        df[col] = clean_numeric(df[col])

df = df.dropna()

if df.empty:
    print("DataFrame is empty after loading and cleaning. No plots will be generated.")
    exit()
if args.conc_mode:
    print("--- Generating Concurrent Mode Latency Histograms ---")
    if not all(col in df.columns for col in ["IODEPTH", "NUMJOBS"]):
        print(
            "Error: IODEPTH and NUMJOBS columns are required for --conc_mode but not found in the data."
        )
        print(
            "Please ensure your LAT_RESULTS_FILE includes these columns when running in concurrent mode."
        )
    else:
        unique_bss_conc = sorted(
            list(df["BS"].unique()), key=parse_block_size_for_sorting
        )
        unique_mixes_conc = df["RW_MIX"].unique()

        for bs_val_c in unique_bss_conc:
            for mix_val_c in unique_mixes_conc:
                subset_bs_mix_c = df[
                    (df["BS"] == bs_val_c) & (df["RW_MIX"] == mix_val_c)
                ]
                if not subset_bs_mix_c.empty:
                    plot_latency_histograms_conc_mode(
                        df_conc=subset_bs_mix_c,
                        bs_val=bs_val_c,
                        mix_val=mix_val_c,
                        is_rewrite=args.rewrite,
                    )
    # Also generate boxplots if conc_mode is on, as they are general
    print("\n--- Generating Latency Boxplots (Concurrent Mode context) ---")
    plot_boxplot_latency()

else:  # Regular plotting mode (non-concurrent)
    print("--- Generating Standard Latency Plots ---")
    # Generate separate latency plots per block size
    plot_metric_by_bs("Avg_LAT", "Средняя общая задержка (мс)", "avg_latency")
    plot_metric_by_bs("Max_LAT", "Максимальная задержка (мс)", "max_latency")
    plot_metric_by_bs("P99_LAT", "99-й перцентиль общей задержки (мс)", "p99_latency")

    # Generate united latency plots (all block sizes together)
    plot_united_metric("Avg_LAT", "Средняя общая задержка (мс)", "avg_latency_united")
    plot_united_metric(
        "Max_LAT", "Максимальная общая задержка (мс)", "max_latency_united"
    )
    plot_united_metric(
        "P99_LAT", "99-й перцентиль общей задержки (мс)", "p99_latency_united"
    )

    # Same for slat
    plot_metric_by_bs(
        "Avg_SLAT", "Средняя задержка отправки запроса (мс)", "avg_slatency"
    )
    plot_metric_by_bs(
        "Max_SLAT", "Максимальная задержка отправки запроса (мс)", "max_slatency"
    )
    plot_metric_by_bs(
        "P99_SLAT", "99-й перцентиль задержки отправки запроса (мс)", "p99_slatency"
    )

    plot_united_metric(
        "Avg_SLAT", "Средняя задержка отправки запроса (мс)", "avg_slatency_united"
    )
    plot_united_metric(
        "Max_SLAT", "Максимальная задержка отправки запроса (мс)", "max_slatency_united"
    )
    plot_united_metric(
        "P99_SLAT",
        "99-й перцентиль задержки отправки запроса (мс)",
        "p99_slatency_united",
    )

    # Same for clat
    plot_metric_by_bs("Avg_CLAT", "Средняя задержка выполнения (мс)", "avg_clatency")
    plot_metric_by_bs(
        "Max_CLAT", "Максимальная задержка выполнения (мс)", "max_clatency"
    )
    plot_metric_by_bs(
        "P99_CLAT", "99-й перцентиль задержки выполнения (мс)", "p99_clatency"
    )

    plot_united_metric(
        "Avg_CLAT", "Средняя задержка выполнения (мс)", "avg_clatency_united"
    )
    plot_united_metric(
        "Max_CLAT", "Максимальная задержка выполнения (мс)", "max_clatency_united"
    )
    plot_united_metric(
        "P99_CLAT", "99-й перцентиль задержки выполнения (мс)", "p99_clatency_united"
    )

    plot_boxplot_latency()

print("\nLatency analysis complete. Graphs saved.")
