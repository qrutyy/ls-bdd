import matplotlib.pyplot as plt
import argparse
import numpy as np
import os
import re
import pandas as pd

RESULTS_FILE = "logs/fio_results.dat"

# Define column names, including IODEPTH and NUMJOBS for conc_mode
# If these are not always present, you might need more sophisticated loading or error handling
lat_column_names = [
    "RunID",
    "DS",
    "BS",
    "RW_MIX",
    "RW_TYPE",
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
    "IODEPTH",
    "NUMJOBS"
]

iops_column_names = [
    "RunID",
    "DS",
    "BS",
    "MIX",
    "BW",
    "IOPS",
    "MODE",
    "RW_TYPE",
    "IODEPTH",
    "NUMJOBS"
]

ds_colors = {
    "sl": "skyblue",
    "ht": "orange",
    "rb": "red",
    "bt": "green"
}

parser = argparse.ArgumentParser(
    description="Generate plots from FIO benchmark results."
)

parser.add_argument(
    "metric",
    help="Metrit to use in plot generation",
)

args = parser.parse_args()

try:
    plot_colors = plt.get_cmap("tab10", 10).colors
except AttributeError:  # Matplotlib < 2.0
    plot_colors = [plt.get_cmap("tab10")(i) for i in np.linspace(0, 1, 10)]


def clean_numeric(series):
    return pd.to_numeric(series, errors="coerce")


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


def get_formatted_ds_name(ds):
    match(ds):
        case "ht":
            return "Хеш-таблица"
        case "sl":
            return "Список с пропусками"
        case "bt":
            return "Б+ дерево"
        case "rb":
            return "Красно-черное дерево"
        case _:
            print("Unknown ds type, check get_formatted_ds_name function in iops_conc_plots.py")
            return "unknown"


def get_metric():
    match(args.metric):
        case "iops":
            return "IOPS"
        case "lat":
            return "P99_LAT"

def plot_general_hist(
    df,
    nj,
    iodepth,
    bs_val,
    metric,
    ds_values=("sl", "ht"),
    save_directory="iops",
    filename="general_iops.png"
):
    """
    Builds 1 plot with 4 pairs of histograms:
      - Read Rand (sl, ht)
      - Read Seq (sl, ht)
      - Write Rand (sl, ht)
      - Write Seq (sl, ht)

    Each pair: two bars side-by-side, one per ds.
    """

    # map workload groups
    workloads = [
        ("0-100", "randrw", "Read Rand"),
        ("0-100", "rw",  "Read Seq"),
        ("100-0", "randrw", "Write Rand"),
        ("100-0", "rw",  "Write Seq"),
    ]

    values_per_group = {label: [] for _, _, label in workloads}

    for mode, rw_type, label in workloads:
        for ds in ds_values:
            row = df[
                (df["NUMJOBS"] == nj) &
                (df["IODEPTH"] == iodepth) &
                (df["BS"] == bs_val) &
                (df["MODE"] == mode) &
                (df["RW_TYPE"] == rw_type) &
                (df["DS"] == ds)
            ]
            if not row.empty:
                values_per_group[label].append(row[metric].median())
            else:
                values_per_group[label].append(0)

    # plotting
    x_labels = [label for _, _, label in workloads]
    x = np.arange(len(x_labels))
    width = 0.35

    plt.figure(figsize=(10, 6))

    # sl bars
    plt.bar(x - width/2,
            [values_per_group[label][0] for label in x_labels],
            width,
            color=ds_colors.get("sl", None),
            label="sl")

    # ht bars
    plt.bar(x + width/2,
            [values_per_group[label][1] for label in x_labels],
            width,
            color=ds_colors.get("ht", None),
            label="ht")

    y_label = "IOPS (тыс. операций/c)" if (metric == "IOPS") else "Общая задержка (мс)"
    title_tmplt = "Медианные значения IOPS" if (metric == "IOPS") else "99-е перцентили общей задержки"
    plt.xticks(x, x_labels)
    plt.ylabel(y_label)
    plt.title(f"{title_tmplt} для различных операций,\nпри NJ={nj}, ID={iodepth}, BS={bs_val}")

    plt.legend()
    plt.tight_layout()

    os.makedirs(save_directory, exist_ok=True)
    save_path = os.path.join(save_directory, filename)
    plt.savefig(save_path)
    plt.close()

    print(f"[ok] Saved general IOPS plot: {save_path}")


def process_df():
    csv_columns = iops_column_names if (args.metric == "IOPS") else lat_column_names
    df = pd.read_csv(
        RESULTS_FILE,
        sep=r"\s+",
        skiprows=0,
        # Ensure all potential columns are named for robust parsing
        names=csv_columns,
        header=None,  # Explicitly state no header row in data to use our names
    )
    print("Original DataFrame head (first 2 rows):")
    print(df.head(2))

    # Centralized numeric cleaning for relevant columns
    cols_to_make_numeric = ["RunID", "IOPS", "IODEPTH", "NUMJOBS"] if (args.metric == "IOPS") else [col for col in df.columns if col not in ["BS", "DS", "RW_MIX", "RW_TYPE"]]

    for col in cols_to_make_numeric:
        if col in df.columns:  # Check if column actually exists from CSV read
            df[col] = clean_numeric(df[col])

    print("\nDataFrame head after cleaning (first 2 rows):")
    print(df.head(2))
    return df


def verify_and_gen_iops_general_plots(metric):
    if df.empty:
        print("DataFrame is empty after loading and cleaning. No plots will be generated.")
        return

    df_m = df[df["MODE"] == metric].copy()

    if not ("IODEPTH" in df_m.columns and "NUMJOBS" in df_m.columns):
        print("IODEPTH and/or NUMJOBS columns are missing. Skipping concurrent average performance bars.")
    else:
        unique_mixes_conc = df_m["MIX"].unique()
        unique_mix_types_conc = df_m["RW_TYPE"].unique()
        assert len(unique_mix_types_conc) > 1
        assert len(unique_mixes_conc) > 1

        unique_nj = df_m["NUMJOBS"].unique()
        unique_id = df_m["IODEPTH"].unique()
        unique_bss_conc = sorted(
            list(df_m["BS"].unique()),
            key=parse_block_size_for_sorting,
        )
        assert len(unique_nj) == 1
        assert len(unique_id) == 1
        assert len(unique_bss_conc) == 1

        plot_general_hist(
            df_for_bs_mix=df_m,
            nj=unique_id[0],
            iodepth=unique_id[0],
            bs_val=unique_bss_conc[0],
            y_metric=metric,
            ds_values=["ht", "sl"], # TODO: pass from plots.sh
            save_directory_base="iops",
            filename_prefix_detail=f"{metric}_general_hist_nj{unique_nj[0]}_id{unique_id[0]}",
        )


df = process_df()

verify_and_gen_iops_general_plots(df, get_metric())
