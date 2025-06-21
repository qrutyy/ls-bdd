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
    help="Indicate rewrite mode (affects titles/paths, e.g., 'с прогревом').",
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
except AttributeError:  # Matplotlib < 2.0
    plot_colors = [plt.get_cmap("tab10")(i) for i in np.linspace(0, 1, 10)]


def parse_block_size_for_sorting(bs_str):
    """
    Parses a block size string and converts it to bytes for sorting.
    @param bs_str: The block size string.
    @return: The block size in bytes.
    """
    bs_str_lower = str(bs_str).lower()
    match = re.match(r"(\d+)([kmgtp]?)b?", bs_str_lower)
    if not match:
        return 0  # Or raise error
    num_part = int(match.group(1))
    unit_part = match.group(2)

    multipliers = {"k": 1024, "m": 1024**2}
    if unit_part:
        return num_part * multipliers.get(unit_part, 1)
    return num_part


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
    @param is_rewrite: Rewrite mode flag for title.
    """
    if df_for_bs_mix.empty:
        print(f"No data for BS={bs_val}, MIX={mix_val} to plot detailed average bars.")
        return

    df_plot_data = df_for_bs_mix.copy()
    if "IODEPTH" in df_plot_data.columns:
        df_plot_data.dropna(
            subset=["IODEPTH"], inplace=True
        )  # Drop rows where IODEPTH is NaN
        df_plot_data["IODEPTH"] = df_plot_data["IODEPTH"].astype(int)
    if "NUMJOBS" in df_plot_data.columns:
        df_plot_data.dropna(
            subset=["NUMJOBS"], inplace=True
        )  # Drop rows where NUMJOBS is NaN
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
    if sorted_avg_data.empty:  # Should not happen if avg_data was not empty
        return

    x_labels = [
        f"ID={int(row['IODEPTH'])}, NJ={int(row['NUMJOBS'])}"
        for _, row in sorted_avg_data.iterrows()
    ]
    y_values = sorted_avg_data[metric_col].tolist()
    if not y_values:  # Should not happen if sorted_avg_data was not empty
        return

    x_positions = range(len(x_labels))
    plt.figure(figsize=(max(10, len(x_labels) * 0.75), 7.5))
    bar_color = "darkslateblue" if "IOPS" in y_axis_label_base else "saddlebrown"
    bars = plt.bar(
        x_positions, y_values, color=bar_color, edgecolor="black", zorder=2, width=0.6
    )

    max_y_val_for_offset = max(y_values) if y_values else 0
    label_format_str = "{:,.2f}" if "Пропускная" in y_axis_label_base else "{:,.0f}"
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

    plt.xlabel("Конфигурация (IODEPTH & NUMJOBS)", fontsize=12, labelpad=15)
    plt.ylabel(f"Среднее {y_axis_label_base}", fontsize=12, labelpad=10)
    title_warmup_status = "(с прогревом)" if is_rewrite else "(без прогрева)"
    title_ds_status = (
        " используя список с пропусками\n," if (DEVICE == "lsvbd1") else ""
    )
    plt.title(
        f"Средние значения IOPS при разной степени параллелизма,{title_ds_status} BS={bs_val}, MIX={mix_val}\n"
        f"на {DEVICE} {title_warmup_status}",
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

    if "IODEPTH" in df_filtered.columns:
        df_filtered.dropna(subset=["IODEPTH"], inplace=True)
        df_filtered["IODEPTH"] = df_filtered["IODEPTH"].astype(int)
    if "NUMJOBS" in df_filtered.columns:
        df_filtered.dropna(subset=["NUMJOBS"], inplace=True)
        df_filtered["NUMJOBS"] = df_filtered["NUMJOBS"].astype(int)

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
            ].copy()  # .copy() already made by boolean indexing
            if subset_bs_mix.empty:
                plt.close()
                continue

            # IODEPTH/NUMJOBS might not exist if FIO config was simple
            id_nj_pairs_exist = (
                "IODEPTH" in subset_bs_mix.columns
                and "NUMJOBS" in subset_bs_mix.columns
            )

            if id_nj_pairs_exist:
                id_nj_pairs = sorted(
                    list(
                        subset_bs_mix[["IODEPTH", "NUMJOBS"]]
                        .drop_duplicates()
                        .itertuples(index=False, name=None)
                    )
                )
            else:  # If no IODEPTH/NUMJOBS, plot the single series for BS/MIX
                id_nj_pairs = [(None, None)]

            for id_val, nj_val in id_nj_pairs:
                if id_nj_pairs_exist:
                    data_for_line = subset_bs_mix[
                        (subset_bs_mix["IODEPTH"] == id_val)
                        & (subset_bs_mix["NUMJOBS"] == nj_val)
                    ].sort_values(by="RunID")
                    label_text = f"ID={id_val}, NJ={nj_val}"
                else:  # No IODEPTH/NUMJOBS columns, use all data for this BS/MIX
                    data_for_line = subset_bs_mix.sort_values(by="RunID")
                    label_text = f"Значения при BS={bs_val}, MIX={mix_val}"

                if not data_for_line.empty and len(data_for_line["RunID"].unique()) > 0:
                    # Aggregate if multiple entries for the same RunID (e.g. if job has multiple sub-jobs summarized)
                    avg_metric_for_runid = (
                        data_for_line.groupby("RunID")[metric_col].mean().reset_index()
                    )

                    plt.plot(
                        avg_metric_for_runid["RunID"],
                        avg_metric_for_runid[metric_col],
                        marker="o",
                        linestyle="-",
                        linewidth=1.5,
                        label=label_text,
                        color=plot_colors[color_idx % len(plot_colors)],
                    )
                    plot_has_data = True
                    color_idx += 1

            if plot_has_data:
                if (
                    id_nj_pairs_exist and len(id_nj_pairs) > 1
                ):  # Only show legend if multiple lines
                    plt.legend(
                        title="Конфигурации (IODEPTH, NUMJOBS)",
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
                plt.xlabel("Номер итерации")
                title_status = "(с прогревом)" if args.rewrite else "(без прогрева)"
                title_ds_status = (
                    ", используя список с пропусками"
                    if (device_name == "lsvbd1")
                    else ""
                )
                title_main = f"Средние значения {metric_col} при различных конфигурациях (ID/NJ){title_ds_status},\nBS={bs_val}, MIX={mix_val} на {DEVICE} {title_status}"
                if not id_nj_pairs_exist:
                    title_main = f"Средние значения {metric_col} при ID=32 NJ=8,\nBS={bs_val}, MIX={mix_val} на {DEVICE} {title_status}"
                plt.title(title_main, fontsize=13)
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

    # Ensure IODEPTH/NUMJOBS are int for iteration and labeling
    id_nj_cols_exist = (
        "IODEPTH" in df_filtered.columns and "NUMJOBS" in df_filtered.columns
    )
    if id_nj_cols_exist:
        df_filtered.dropna(subset=["IODEPTH", "NUMJOBS"], inplace=True)
        df_filtered["IODEPTH"] = df_filtered["IODEPTH"].astype(int)
        df_filtered["NUMJOBS"] = df_filtered["NUMJOBS"].astype(int)
        grouping_cols = ["MIX", "IODEPTH", "NUMJOBS"]
    else:
        grouping_cols = ["MIX"]

    # Iterate over unique (MIX, IODEPTH, NUMJOBS) combinations or just MIX if ID/NJ not present
    unique_configs = df_filtered[grouping_cols].drop_duplicates()

    for _, row_config in unique_configs.iterrows():
        mix_val = row_config["MIX"]
        id_val = row_config["IODEPTH"] if id_nj_cols_exist else None
        nj_val = row_config["NUMJOBS"] if id_nj_cols_exist else None

        if id_nj_cols_exist:
            df_fixed_config = df_filtered[
                (df_filtered["MIX"] == mix_val)
                & (df_filtered["IODEPTH"] == id_val)
                & (df_filtered["NUMJOBS"] == nj_val)
            ].copy()
        else:
            df_fixed_config = df_filtered[df_filtered["MIX"] == mix_val].copy()

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
        label_format_str = "{:,.2f}" if "Пропускная" in y_axis_label_base else "{:,.0f}"
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

        plt.xlabel("Размера блока (BS)", fontsize=12, labelpad=15)
        plt.ylabel(f"{y_axis_label_base}", fontsize=12, labelpad=10)
        title_warmup_status = "(с прогревом)" if args.rewrite else "(без прогрева)"
        title_ds_status = (
            ",\nиспользуя список с пропусками" if (DEVICE == "lsvbd1") else ""
        )

        title_config_part = f"MIX={mix_val}"
        if id_nj_cols_exist:
            title_config_part += f", ID={id_val}, NJ={nj_val}"

        plt.title(
            f"Средние значения при различных BS{title_ds_status} для {title_config_part}\n"
            f"на {DEVICE} {title_warmup_status}",
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
        filename_config_part = f"{safe_mix}"
        if id_nj_cols_exist:
            filename_config_part += f"_id{id_val}_nj{nj_val}"
        save_path = os.path.join(
            output_dir,
            f"{metric_fn_part}_bs_compare_{filename_config_part}.png",
        )

        try:
            plt.savefig(save_path)
            print(f"Saved: {save_path}")
        except Exception as e:
            print(f"Error saving plot {save_path}: {e}")
        finally:
            plt.close()


# NEW PLOTTING FUNCTION as requested
def plot_non_conc_bs_comparison_over_runs(
    df_data, metric_col, y_axis_label, plot_subdir_name, device_name, is_rewrite
):
    """
    For non-concurrent mode: Generates line plots comparing performance across different Block Sizes
    over RunID for each fixed MIX. One plot per MIX, lines are for BS.
    Based on the example image provided by the user.

    @param df_data: DataFrame containing all relevant data.
    @param metric_col: Metric column ("IOPS" or "BW").
    @param y_axis_label: Y-axis label.
    @param plot_subdir_name: Subdirectory under PLOTS_PATH (e.g., "iops" or "bw").
    @param device_name: Device name for title.
    @param is_rewrite: Rewrite mode flag for title.
    """
    mode_filter = "tp" if metric_col == "BW" else "iops"
    df_filtered = df_data[df_data["MODE"] == mode_filter].copy()

    if df_filtered.empty:
        print(
            f"No data (MODE='{mode_filter}') for BS comparison over runs in {plot_subdir_name}."
        )
        return

    id_nj_cols_exist = (
        "IODEPTH" in df_filtered.columns and "NUMJOBS" in df_filtered.columns
    )
    if id_nj_cols_exist:
        # Attempt to convert to int, fillna or dropna if there are issues from clean_numeric
        df_filtered.dropna(
            subset=["IODEPTH", "NUMJOBS"], inplace=True
        )  # Essential for these annotations
        df_filtered["IODEPTH"] = df_filtered["IODEPTH"].astype(int)
        df_filtered["NUMJOBS"] = df_filtered["NUMJOBS"].astype(int)

    unique_mixes = df_filtered["MIX"].unique()

    for mix_val in unique_mixes:
        df_current_mix = df_filtered[df_filtered["MIX"] == mix_val].copy()
        if df_current_mix.empty:
            continue

        id_nj_annotation = ""
        if id_nj_cols_exist:
            unique_id_nj_for_mix = df_current_mix[
                ["IODEPTH", "NUMJOBS"]
            ].drop_duplicates()
            if len(unique_id_nj_for_mix) == 1:
                id_val = unique_id_nj_for_mix.iloc[0]["IODEPTH"]
                nj_val = unique_id_nj_for_mix.iloc[0]["NUMJOBS"]
                id_nj_annotation = f", ID={id_val}, NJ={nj_val}"
            elif len(unique_id_nj_for_mix) > 1:
                id_nj_annotation = ", Varying ID/NJ"

        plt.figure(figsize=(10, 6))  # Adjusted figsize to be similar to example
        plot_has_data = False
        color_idx = 0

        unique_bss_for_mix = sorted(
            list(df_current_mix["BS"].unique()), key=parse_block_size_for_sorting
        )

        for bs_val in unique_bss_for_mix:
            data_for_bs_line = df_current_mix[
                df_current_mix["BS"] == bs_val
            ]  # No .copy needed due to indexing

            avg_metric_per_run = data_for_bs_line.groupby("RunID", as_index=False)[
                metric_col
            ].mean()
            avg_metric_per_run = avg_metric_per_run.sort_values(by="RunID")

            if not avg_metric_per_run.empty:
                plt.plot(
                    avg_metric_per_run["RunID"],
                    avg_metric_per_run[metric_col],
                    marker="o",
                    linestyle="-",
                    linewidth=1.5,
                    label=f"BS={bs_val}, MIX={mix_val}",  # Legend format from example
                    color=plot_colors[color_idx % len(plot_colors)],
                )
                plot_has_data = True
                color_idx += 1

        if plot_has_data:
            plt.legend(
                fontsize="small", loc="best", frameon=True
            )  # Example doesn't have legend title

            output_dir = os.path.join(PLOTS_PATH, plot_subdir_name)
            os.makedirs(output_dir, exist_ok=True)

            safe_mix_val = str(mix_val).replace("/", "_")
            metric_fn_part = "bw" if metric_col == "BW" else "iops"
            save_path = os.path.join(
                output_dir, f"{metric_fn_part}_runs_by_bs_{safe_mix_val}.png"
            )

            plt.ylabel(y_axis_label, fontsize=10)
            plt.xlabel("Номер итерации", fontsize=10)

            title_status = "(с прогревом)" if is_rewrite else "(без прогрева)"
            title_metric_name = (
                "Средняя пропускная способность" if metric_col == "BW" else metric_col
            )
            title_ds_status = (
                "используя список с пропусками\n," if (DEVICE == "lsvbd1") else ""
            )

            main_title = f"{title_metric_name} {title_ds_status}при MIX={mix_val} на {device_name} {title_status}"
            # Adding IODEPTH/NUMJOBS to title if consistent and known for this MIX
            if id_nj_annotation and id_nj_annotation != ", Varying ID/NJ":
                main_title += f"\n(Конфигурация: {id_nj_annotation.strip(', ')})"

            plt.title(main_title, fontsize=12, pad=10)  # Adjusted padding

            plt.xticks(fontsize=9)
            plt.yticks(fontsize=9)
            plt.grid(True, linestyle="--", alpha=0.7)
            plt.tight_layout()

            try:
                plt.savefig(save_path)
                print(f"Saved: {save_path}")
            except Exception as e:
                print(f"Error saving plot {save_path}: {e}")
            finally:
                plt.close()
        else:
            plt.close()


df = pd.read_csv(
    RESULTS_FILE,
    sep=r"\s+",
    skiprows=0,
    # Ensure all potential columns are named for robust parsing
    names=["RunID", "BS", "MIX", "BW", "IOPS", "MODE", "IODEPTH", "NUMJOBS"],
    header=None,  # Explicitly state no header row in data to use our names
)
print("Original DataFrame head (first 2 rows):")
print(df.head(2))

# Centralized numeric cleaning for relevant columns
cols_to_make_numeric = ["RunID", "BW", "IOPS"]
if "IODEPTH" in df.columns:
    cols_to_make_numeric.append("IODEPTH")
if "NUMJOBS" in df.columns:
    cols_to_make_numeric.append("NUMJOBS")

for col in cols_to_make_numeric:
    if col in df.columns:  # Check if column actually exists from CSV read
        df[col] = clean_numeric(df[col])

# Define essential columns that must not be NaN for any plot type
essential_cols_for_dropna = ["RunID", "BS", "MIX", "BW", "IOPS", "MODE"]
df = df.dropna(subset=essential_cols_for_dropna)


print("\nDataFrame head after cleaning (first 2 rows):")
print(df.head(2))


if df.empty:
    print("DataFrame is empty after loading and cleaning. No plots will be generated.")
else:
    metric_col_main = "BW" if args.tp else "IOPS"
    y_label_main = (
        "Пропускная способность (ГБ/с)" if args.tp else "IOPS (тыс. операций/c)"
    )
    metric_prefix_main = "bw" if args.tp else "iops"

    if args.conc_mode:
        print(
            f"\n--- Generating Concurrent Mode {metric_prefix_main.upper()} Plots ---"
        )
        conc_runs_detailed_dir = f"{metric_prefix_main}_conc_runs"
        print(
            f"\nGenerating concurrent {metric_prefix_main} multi-line run plots (lines per ID/NJ for each BS/MIX)..."
        )
        plot_metric_multiline_runs(
            df.copy(), metric_col_main, y_label_main, conc_runs_detailed_dir
        )

        conc_detailed_bars_dir = f"{metric_prefix_main}_conc_avg_bars_idnj"
        print(
            f"\nGenerating concurrent {metric_prefix_main} average performance bars (ID/NJ based)..."
        )
        mode_filter_conc = "tp" if args.tp else "iops"
        df_for_conc_plots = df[df["MODE"] == mode_filter_conc].copy()

        if df_for_conc_plots.empty:
            print(
                f"No data with MODE='{mode_filter_conc}' for concurrent average performance bars."
            )
        else:
            # Ensure IODEPTH/NUMJOBS are present for these plots
            if not (
                "IODEPTH" in df_for_conc_plots.columns
                and "NUMJOBS" in df_for_conc_plots.columns
            ):
                print(
                    "IODEPTH and/or NUMJOBS columns are missing. Skipping concurrent average performance bars."
                )
            else:
                unique_bss_conc = sorted(
                    list(df_for_conc_plots["BS"].unique()),
                    key=parse_block_size_for_sorting,
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
                                filename_prefix_detail=f"{metric_prefix_main}_avg_bars_idnj",
                                is_rewrite=args.rewrite,
                            )
    else:  # Non-concurrent mode
        print(
            f"\n--- Generating Non-Concurrent Mode {metric_prefix_main.upper()} Plots ---"
        )

        non_conc_plot_subdir = f"{metric_prefix_main}"

        # Plot Type 1 (Non-conc): Metric vs RunID, one line per ID/NJ, for each BS/MIX
        print(
            f"\nGenerating non-concurrent {metric_prefix_main} multi-line run plots (lines per ID/NJ for each BS/MIX)..."
        )
        plot_metric_multiline_runs(
            df.copy(), metric_col_main, y_label_main, non_conc_plot_subdir
        )

        # NEW PLOT TYPE (Non-conc): Metric vs RunID, one line per BS, for each MIX
        print(
            f"\nGenerating non-concurrent {metric_prefix_main} multi-line run plots (lines per BS for each MIX)..."
        )
        plot_non_conc_bs_comparison_over_runs(
            df.copy(),
            metric_col_main,
            y_label_main,
            non_conc_plot_subdir,
            DEVICE,
            args.rewrite,
        )

        # Plot Type 2 (Non-conc): Bar charts (Avg Metric vs BS for each fixed MIX/ID/NJ)
        print(
            f"\nGenerating non-concurrent {metric_prefix_main} block size comparison bars (avg over runs)..."
        )
        plot_non_conc_bs_comparison_bars(
            df.copy(), metric_col_main, y_label_main, non_conc_plot_subdir
        )

    print("\nAnalysis complete. Graphs saved (if any data was available).")
