import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os
import argparse
import re

parser = argparse.ArgumentParser(
    description="Generate plots from FIO benchmark results."
)
parser.add_argument(
    "--raw",
    action="store_true",
    help="Save plots to the 'raw' subdirectory (vs 'vbd').",
)
parser.add_argument(
    "--tp",
    action="store_true",
    help="Generate throughput (BW) plots instead of IOPS plots.",
)
parser.add_argument(
    "--rewrite",
    action="store_true",
    help="Indicate rewrite mode (affects titles/paths, e.g., 'with warm up').",
)
parser.add_argument(
    "--conc_mode",
    action="store_true",
    help="Generate plots specific to concurrent mode evaluation.",
)
args = parser.parse_args()

RESULTS_FILE = "logs/fio_results.dat"
PLOTS_PATH = "./plots/avg/raw" if args.raw else "./plots/avg/vbd"
PLOTS_PATH = PLOTS_PATH + "/rewrite" if args.rewrite else PLOTS_PATH + "/non_rewrite"
DEVICE = "nullb0" if args.raw else "lsvbd1"

try:
    plot_colors = plt.get_cmap("tab10", 10).colors
except AttributeError:
    plot_colors = [plt.get_cmap("tab10")(i) for i in np.linspace(0, 1, 10)]


def parse_block_size_for_sorting(bs_str):
    """
    Parses a block size string and converts it to bytes for sorting.
    @param bs_str: The block size string.
    @return: The block size in bytes.
    """
    bs_str_lower = str(bs_str).lower()
    match = re.match(r"(\d+)([kmgtpbs]*)", bs_str_lower)
    if not match:
        return 0
    num_part = int(match.group(1))
    unit_part = match.group(2)
    return num_part * 1024


def clean_numeric(series):
    return pd.to_numeric(series, errors="coerce")


def plot_iodepth_numjobs_avg_bars(
    df_for_bs_mix,
    metric_col,
    y_axis_label_base,
    bs_val,
    mix_val,
    save_directory_base,
    filename_prefix_detail,
    device_name,
    is_rewrite,
):
    """
    Generates a bar chart: Average metric vs. (IODEPTH, NUMJOBS) for a specific (BS, MIX).
    Primarily for concurrent mode.

    @param df_for_bs_mix: DataFrame filtered for a specific BS and MIX.
    @param metric_col: Metric column name ("IOPS" or "BW").
    @param y_axis_label_base: Y-axis base label.
    @param bs_val: Current block size.
    @param mix_val: Current R/W mix.
    @param save_directory_base: Base directory under PLOTS_PATH.
    @param filename_prefix_detail: Prefix for filename.
    @param device_name: Device name for title.
    @param is_rewrite: Rewrite mode flag for title.
    """
    if df_for_bs_mix.empty:
        print(f"No data for BS={bs_val}, MIX={mix_val} to plot detailed average bars.")
        return

    df_plot_data = df_for_bs_mix.copy()
    df_plot_data["IODEPTH"] = df_plot_data["IODEPTH"].astype(int)
    df_plot_data["NUMJOBS"] = df_plot_data["NUMJOBS"].astype(int)
    avg_data = df_plot_data.groupby(["IODEPTH", "NUMJOBS"], as_index=False)[
        metric_col
    ].mean()

    if avg_data.empty:
        print(
            f"No data after grouping for BS={bs_val}, MIX={mix_val}, Metric={metric_col} for detailed average bars."
        )
        return

    sorted_avg_data = avg_data.sort_values(by=["IODEPTH", "NUMJOBS"])
    if sorted_avg_data.empty:
        return

    x_labels = [
        f"ID={int(row['IODEPTH'])}, NJ={int(row['NUMJOBS'])}"
        for _, row in sorted_avg_data.iterrows()
    ]
    y_values = sorted_avg_data[metric_col].tolist()
    if not y_values:
        return

    x_positions = range(len(x_labels))
    plt.figure(figsize=(max(10, len(x_labels) * 0.75), 7.5))
    bar_color = "darkslateblue" if "IOPS" in y_axis_label_base else "saddlebrown"
    bars = plt.bar(
        x_positions, y_values, color=bar_color, edgecolor="black", zorder=2, width=0.6
    )

    max_y_val_for_offset = max(y_values) if y_values else 0
    label_format_str = "{:,.2f}" if "Bandwidth" in y_axis_label_base else "{:,.0f}"
    for bar in bars:
        yval = bar.get_height()
        text_offset = max_y_val_for_offset * 0.02
        plt.text(
            bar.get_x() + bar.get_width() / 2.0,
            yval + text_offset,
            label_format_str.format(yval),
            ha="center",
            va="bottom",
            fontsize=7.5,
            rotation=0,
        )

    plt.xlabel("Configuration (IODEPTH & NUMJOBS)", fontsize=12, labelpad=15)
    plt.ylabel(f"Average {y_axis_label_base}", fontsize=12, labelpad=10)
    title_warmup_status = "(with warm up)" if is_rewrite else "(without warm up)"
    plt.title(
        f"Average Performance by ID/NJ for BS={bs_val}, MIX={mix_val}\n"
        f"on {device_name} {title_warmup_status}",
        fontsize=13,
        pad=20,
    )
    plt.xticks(x_positions, x_labels, rotation=45, ha="right", fontsize=9)
    plt.yticks(fontsize=9)
    plt.grid(axis="y", linestyle="--", alpha=0.7, zorder=1)
    plt.gca().spines["top"].set_visible(False)
    plt.gca().spines["right"].set_visible(False)
    plt.tight_layout()

    output_dir = os.path.join(PLOTS_PATH, save_directory_base)
    os.makedirs(output_dir, exist_ok=True)
    safe_bs_val = str(bs_val).replace("/", "_")
    safe_mix_val = str(mix_val).replace("/", "_")
    save_path = os.path.join(
        output_dir, f"{filename_prefix_detail}_{safe_mix_val}_{safe_bs_val}.png"
    )

    try:
        plt.savefig(save_path)
        print(f"Saved: {save_path}")
    except Exception as e:
        print(f"Error saving plot {save_path}: {e}")
    finally:
        plt.close()


def plot_metric_multiline_runs(df_plot, metric_col, y_axis_label, plot_subdir_name):
    """
    Generates multi-line plots: Metric vs. RunID, one line per (IODEPTH, NUMJOBS).
    One plot per (BS, MIX). Used for concurrent and non-concurrent modes.

    @param df_plot: DataFrame.
    @param metric_col: Metric column ("IOPS" or "BW").
    @param y_axis_label: Y-axis label.
    @param plot_subdir_name: Subdirectory under PLOTS_PATH.
    """
    mode_filter = "tp" if metric_col == "BW" else "iops"
    df_filtered = df_plot[df_plot["MODE"] == mode_filter].copy()
    if df_filtered.empty:
        print(
            f"No data (MODE='{mode_filter}') for multi-line {metric_col} plots in {plot_subdir_name}."
        )
        return

    unique_bss = sorted(
        list(df_filtered["BS"].unique()), key=parse_block_size_for_sorting
    )
    unique_mixes = df_filtered["MIX"].unique()

    for bs_val in unique_bss:
        for mix_val in unique_mixes:
            plt.figure(figsize=(12, 7))
            plot_has_data = False
            color_idx = 0
            subset_bs_mix = df_filtered[
                (df_filtered["BS"] == bs_val) & (df_filtered["MIX"] == mix_val)
            ].copy()
            if subset_bs_mix.empty:
                plt.close()
                continue

            subset_bs_mix["IODEPTH"] = subset_bs_mix["IODEPTH"].astype(int)
            subset_bs_mix["NUMJOBS"] = subset_bs_mix["NUMJOBS"].astype(int)
            id_nj_pairs = sorted(
                list(
                    subset_bs_mix[["IODEPTH", "NUMJOBS"]]
                    .drop_duplicates()
                    .itertuples(index=False, name=None)
                )
            )

            for id_val, nj_val in id_nj_pairs:
                data_for_line = subset_bs_mix[
                    (subset_bs_mix["IODEPTH"] == id_val)
                    & (subset_bs_mix["NUMJOBS"] == nj_val)
                ].sort_values(by="RunID")
                if not data_for_line.empty and len(data_for_line["RunID"].unique()) > 0:
                    if (
                        len(data_for_line["RunID"].unique()) == 1
                        and len(data_for_line) > 1
                    ):
                        avg_metric_for_runid = (
                            data_for_line.groupby("RunID")[metric_col]
                            .mean()
                            .reset_index()
                        )
                        plt.plot(
                            avg_metric_for_runid["RunID"],
                            avg_metric_for_runid[metric_col],
                            marker="o",
                            linestyle="-",
                            linewidth=1.5,
                            label=f"ID={id_val}, NJ={nj_val}",
                            color=plot_colors[color_idx % len(plot_colors) + 1],
                        )
                    else:
                        plt.plot(
                            data_for_line["RunID"],
                            data_for_line[metric_col],
                            marker="o",
                            linestyle="-",
                            linewidth=1.5,
                            label=f"ID={id_val}, NJ={nj_val}",
                            color=plot_colors[color_idx % len(plot_colors)],
                        )
                    plot_has_data = True
                    color_idx += 1

            if plot_has_data:
                plt.legend(
                    title="Config (IODEPTH, NUMJOBS)",
                    fontsize="small",
                    loc="best",
                    frameon=True,
                )
                output_dir = os.path.join(PLOTS_PATH, plot_subdir_name)
                os.makedirs(output_dir, exist_ok=True)
                safe_bs = str(bs_val).replace("/", "_")
                safe_mix = str(mix_val).replace("/", "_")
                metric_fn_part = "bw" if metric_col == "BW" else "iops"
                save_path = os.path.join(
                    output_dir, f"{metric_fn_part}_runs_detail_{safe_bs}_{safe_mix}.png"
                )
                plt.ylabel(y_axis_label)
                plt.xlabel("Run number")
                title_status = "(with warm up)" if args.rewrite else "(without warm up)"
                plt.title(
                    f"{metric_col} per Run by Configuration for BS={bs_val}, MIX={mix_val}\non {DEVICE} {title_status}",
                    fontsize=13,
                )
                plt.grid(True, linestyle="--", alpha=0.7)
                plt.tight_layout()
                plt.savefig(save_path)
                print(f"Saved: {save_path}")
            plt.close()


def plot_non_conc_bs_comparison_bars(
    df_data, metric_col, y_axis_label_base, plot_subdir_name
):
    """
    For non-concurrent mode: Generates bar charts comparing performance across different Block Sizes
    for each fixed (MIX, IODEPTH, NUMJOBS) configuration.
    One plot per (MIX, IODEPTH, NUMJOBS). X-axis is Block Size.

    @param df_data: DataFrame containing all relevant data. @type df_data: pd.DataFrame
    @param metric_col: Metric column ("IOPS" or "BW"). @type metric_col: str
    @param y_axis_label_base: Y-axis base label. @type y_axis_label_base: str
    @param plot_subdir_name: Subdirectory under PLOTS_PATH (e.g., "iops_bs_comparison"). @type plot_subdir_name: str
    """
    mode_filter = "tp" if metric_col == "BW" else "iops"
    df_filtered = df_data[df_data["MODE"] == mode_filter].copy()
    if df_filtered.empty:
        print(
            f"No data (MODE='{mode_filter}') for BS comparison bars in {plot_subdir_name}."
        )
        return

    df_filtered["IODEPTH"] = df_filtered["IODEPTH"].astype(int)
    df_filtered["NUMJOBS"] = df_filtered["NUMJOBS"].astype(int)

    # Iterate over unique (MIX, IODEPTH, NUMJOBS) combinations
    unique_configs = df_filtered[["MIX", "IODEPTH", "NUMJOBS"]].drop_duplicates()

    for _, row_config in unique_configs.iterrows():
        mix_val = row_config["MIX"]
        id_val = row_config["IODEPTH"]
        nj_val = row_config["NUMJOBS"]

        # Filter data for the current fixed (MIX, IODEPTH, NUMJOBS)
        df_fixed_config = df_filtered[
            (df_filtered["MIX"] == mix_val)
            & (df_filtered["IODEPTH"] == id_val)
            & (df_filtered["NUMJOBS"] == nj_val)
        ].copy()

        if df_fixed_config.empty:
            continue

        # Group by Block Size and calculate average metric
        avg_data_by_bs = df_fixed_config.groupby("BS", as_index=False)[
            metric_col
        ].mean()
        if avg_data_by_bs.empty:
            continue

        avg_data_by_bs["sortable_bs"] = avg_data_by_bs["BS"].apply(
            parse_block_size_for_sorting
        )
        sorted_avg_data = avg_data_by_bs.sort_values(by="sortable_bs")
        if sorted_avg_data.empty:
            continue

        x_labels = sorted_avg_data["BS"].tolist()
        y_values = sorted_avg_data[metric_col].tolist()
        if not y_values:
            continue

        x_positions = range(len(x_labels))
        plt.figure(figsize=(max(8, len(x_labels) * 0.8), 7))
        bar_color = "olivedrab" if "IOPS" in y_axis_label_base else "chocolate"
        bars = plt.bar(
            x_positions,
            y_values,
            color=bar_color,
            edgecolor="black",
            zorder=2,
            width=0.5,
        )

        max_y_val_for_offset = max(y_values) if y_values else 0
        label_format_str = "{:,.2f}" if "Bandwidth" in y_axis_label_base else "{:,.0f}"
        for bar in bars:
            yval = bar.get_height()
            text_offset = max_y_val_for_offset * 0.02
            plt.text(
                bar.get_x() + bar.get_width() / 2.0,
                yval + text_offset,
                label_format_str.format(yval),
                ha="center",
                va="bottom",
                fontsize=8,
                rotation=0,
            )

        plt.xlabel("Block Size (BS)", fontsize=12, labelpad=15)
        plt.ylabel(f"Average {y_axis_label_base}", fontsize=12, labelpad=10)
        title_warmup_status = "(with warm up)" if args.rewrite else "(without warm up)"
        plt.title(
            f"Avg Performance by Block Size for MIX={mix_val}, ID={id_val}, NJ={nj_val}\n"
            f"({len(x_labels)} block sizes) on {DEVICE} {title_warmup_status}",
            fontsize=13,
            pad=20,
        )
        plt.xticks(x_positions, x_labels, rotation=45, ha="right", fontsize=9)
        plt.yticks(fontsize=9)
        plt.grid(axis="y", linestyle="--", alpha=0.7, zorder=1)
        plt.gca().spines["top"].set_visible(False)
        plt.gca().spines["right"].set_visible(False)
        plt.tight_layout()

        output_dir = os.path.join(PLOTS_PATH, plot_subdir_name)
        os.makedirs(output_dir, exist_ok=True)
        safe_mix = str(mix_val).replace("/", "_")
        metric_fn_part = "bw" if metric_col == "BW" else "iops"
        save_path = os.path.join(
            output_dir,
            f"{metric_fn_part}_bs_compare_{safe_mix}_id{id_val}_nj{nj_val}.png",
        )

        try:
            plt.savefig(save_path)
            print(f"Saved: {save_path}")
        except Exception as e:
            print(f"Error saving plot {save_path}: {e}")
        finally:
            plt.close()


df = pd.read_csv(
    RESULTS_FILE,
    sep=r"\s+",
    skiprows=0,
    names=["RunID", "BS", "MIX", "BW", "IOPS", "MODE", "IODEPTH", "NUMJOBS"],
)
print("Original DataFrame head (first 2 rows):")
print(df.head(2))

df["BW"] = clean_numeric(df["BW"])
df["IOPS"] = clean_numeric(df["IOPS"])
df["IODEPTH"] = clean_numeric(df["IODEPTH"])
df["NUMJOBS"] = clean_numeric(df["NUMJOBS"])
df["RunID"] = clean_numeric(df["RunID"])

df = df.dropna(
    subset=["RunID", "BS", "MIX", "BW", "IOPS", "MODE", "IODEPTH", "NUMJOBS"]
)

print("\nDataFrame head after cleaning (first 2 rows):")
print(df.head(2))


if df.empty:
    print("DataFrame is empty after loading and cleaning. No plots will be generated.")
else:
    metric_col_main = "BW" if args.tp else "IOPS"
    y_label_main = "Bandwidth (GB/s)" if args.tp else "IOPS (K/s)"  # Confirm units
    metric_prefix_main = "bw" if args.tp else "iops"

    if args.conc_mode:
        print(
            f"\n--- Generating Concurrent Mode {metric_prefix_main.upper()} Plots ---"
        )

        # Plot Type 1: Multi-line plots (Metric vs RunID, one line per ID/NJ for each BS/MIX)
        print(f"\nGenerating concurrent {metric_prefix_main} multi-line run plots...")
        conc_runs_detailed_dir = f"{metric_prefix_main}_conc"  # Changed dir name
        plot_metric_multiline_runs(
            df.copy(), metric_col_main, y_label_main, conc_runs_detailed_dir
        )

        # Plot Type 2: Bar charts (Average Metric vs ID/NJ for each BS/MIX)
        print(
            f"\nGenerating concurrent {metric_prefix_main} average performance bars (ID/NJ based)..."
        )
        conc_detailed_bars_dir = (
            f"{metric_prefix_main}_conc_detailed_bars"  # Changed dir name
        )

        mode_filter_conc = "tp" if args.tp else "iops"
        df_for_conc_plots = df[df["MODE"] == mode_filter_conc].copy()

        if df_for_conc_plots.empty:
            print(
                f"No data with MODE='{mode_filter_conc}' for concurrent average performance bars."
            )
        else:
            unique_bss_conc = sorted(
                list(df_for_conc_plots["BS"].unique()), key=parse_block_size_for_sorting
            )
            unique_mixes_conc = df_for_conc_plots["MIX"].unique()
            for bs_val_c in unique_bss_conc:
                for mix_val_c in unique_mixes_conc:
                    subset_bs_mix_c = df_for_conc_plots[
                        (df_for_conc_plots["BS"] == bs_val_c)
                        & (df_for_conc_plots["MIX"] == mix_val_c)
                    ]
                    if not subset_bs_mix_c.empty:
                        plot_iodepth_numjobs_avg_bars(
                            df_for_bs_mix=subset_bs_mix_c,
                            metric_col=metric_col_main,
                            y_axis_label_base=y_label_main,
                            bs_val=bs_val_c,
                            mix_val=mix_val_c,
                            save_directory_base=conc_detailed_bars_dir,
                            filename_prefix_detail=f"{metric_prefix_main}_avg_bars_idnj",  # Clarified filename
                            device_name=DEVICE,
                            is_rewrite=args.rewrite,
                        )
    else:
        print(
            f"\n--- Generating Non-Concurrent Mode {metric_prefix_main.upper()} Plots ---"
        )

        # Plot Type 1 (Non-conc): Multi-line runs (Metric vs RunID, one line per ID/NJ for each BS/MIX)
        non_conc_runs_detailed_dir = f"{metric_prefix_main}"
        print(
            f"\nGenerating non-concurrent {metric_prefix_main} multi-line run plots..."
        )
        plot_metric_multiline_runs(
            df.copy(), metric_col_main, y_label_main, non_conc_runs_detailed_dir
        )

        # Plot Type 2 (Non-conc): Bar charts (Avg Metric vs BS for each fixed MIX/ID/NJ)
        non_conc_bs_comparison_dir = f"{metric_prefix_main}"
        print(
            f"\nGenerating non-concurrent {metric_prefix_main} block size comparison bars..."
        )
        plot_non_conc_bs_comparison_bars(
            df.copy(), metric_col_main, y_label_main, non_conc_bs_comparison_dir
        )

    print("\nAnalysis complete. Graphs saved (if any data was available).")
