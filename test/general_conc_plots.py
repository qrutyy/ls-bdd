import matplotlib.pyplot as plt
import argparse
import numpy as np
import os
import re
import pandas as pd
import config_parsers as cfg_parser

RESULTS_FILE_PATH = "logs/fio_results.dat"

# Predefined defaults
DEFAULT_DS_MAPPING = {
    "ht": "Hash-table",
    "sl": "Skiplist",
    "bt": "B+ tree",
    "rb": "Red-Black tree",
}

# Define column names, including IODEPTH and NUMJOBS for conc_mode
# If these are not always present, you might need more sophisticated loading or error handling
LAT_COLUMN_NAMES = [
    "RunID",
    "DS",
    "BS",
    "RW_MIX",
    "RW_TYPE",
    "MODE",
    "Avg_SLAT",
    "Avg_CLAT",
    "Avg_LAT",
    "Max_SLAT",
    "Max_CLAT",
    "Max_LAT",
    "P99_SLAT",
    "P99_CLAT",
    "P99_LAT",
    "IODEPTH",
    "NUMJOBS",
]

IOPS_COLUMN_NAMES = [
    "RunID",
    "DS",
    "BS",
    "RW_MIX",
    "BW",
    "IOPS",
    "MODE",
    "RW_TYPE",
    "IODEPTH",
    "NUMJOBS",
]

DS_COLORS = {"sl": "steelblue", "ht": "indianred", "rb": "seagreen", "bt": "darkkhaki"}
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

PLOT_TITLES = cfg_parser.parse_plot_titles()

# Load user mapping from shell file
USER_DS_MAPPING = cfg_parser.load_ds_mapping()

# Merge: user mapping overrides defaults
DS_MAPPING = {**DEFAULT_DS_MAPPING, **USER_DS_MAPPING}


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
    """
    Return the formatted name using merged mapping (defaults + PY_PL_NEW_DS).
    """
    if ds in DS_MAPPING:
        return DS_MAPPING[ds]
    else:
        print("Unknown ds type, check PY_PL_NEW_DS in configurable_params.sh")
        return "unknown"


# ill just leave it here pog
# ps: child of regression
def get_metric():
    match (args.metric):
        case "IOPS":
            return "IOPS"
        case "LAT":
            return "LAT"


def plot_general_hist(
    df,
    nj,
    iodepth,
    bs_val,
    metric,
    ds_values=("sl", "ht"),
    save_directory="iops",
    filename="general_iops.png",
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
        ("0-100", "randrw", "Random read"),
        ("0-100", "rw", "Sequential read"),
        ("100-0", "randrw", "Random write"),
        ("100-0", "rw", "Sequential write"),
    ]

    values_per_group = {label: [] for _, _, label in workloads}

    for mix, rw_type, label in workloads:
        for ds in ds_values:
            row = df[
                (df["NUMJOBS"] == nj)
                & (df["IODEPTH"] == iodepth)
                & (df["BS"] == bs_val)
                & (df["RW_MIX"] == mix)
                & (df["RW_TYPE"] == rw_type)
                & (df["DS"] == ds)
            ]
            if not row.empty:
                if metric == "LAT":
                    values_per_group[label].append(row["P99_LAT"].median())
                else:
                    values_per_group[label].append(row[metric].median())
            else:
                values_per_group[label].append(0)

    # plotting
    x_labels = [label for _, _, label in workloads]
    x = np.arange(len(x_labels))
    width = 0.3

    plt.figure(figsize=(10, 6))

    for idx, ds in enumerate(ds_values):
        plt.bar(
            x - width / 2 + idx * width / len(ds_values),
            [values_per_group[label][idx] for label in x_labels],
            width / len(ds_values),
            color=DS_COLORS.get(ds, None),
            label=get_formatted_ds_name(ds),
        )

    y_label = (
        PLOT_TITLES["PL_GENERAL_IOPS_Y_TITLE"]
        if (metric == "IOPS")
        else PLOT_TITLES["PL_GENERAL_LAT_Y_TITLE"]
    )
    title_tmplt = (
        PLOT_TITLES["PL_GENERAL_IOPS_TITLE_TEMPLATE"]
        if (metric == "IOPS")
        else PLOT_TITLES["PL_GENERAL_LAT_TITLE_TEMPLATE"]
    )
    plt.xticks(x, x_labels)
    plt.ylabel(y_label)
    plt.title(
        f"{title_tmplt} for different operations,\nNJ={nj}, ID={iodepth}, BS={bs_val}K"
    )

    plt.legend()
    plt.tight_layout()

    directory = "iops" if (metric == "IOPS") else "latency"
    save_directory = save_directory + directory

    os.makedirs(save_directory, exist_ok=True)
    save_path = os.path.join(save_directory, filename)
    plt.savefig(save_path)
    plt.close()

    print(f"[ok] Saved general IOPS plot: {save_path}")


def process_df():
    csv_columns = IOPS_COLUMN_NAMES if (args.metric == "IOPS") else LAT_COLUMN_NAMES
    df = pd.read_csv(
        RESULTS_FILE_PATH,
        sep=r"\s+",
        skiprows=0,
        # Ensure all potential columns are named for robust parsing
        names=csv_columns,
        header=None,  # Explicitly state no header row in data to use our names
    )
    print("Original DataFrame head (first 2 rows):")
    print(df)

    # Centralized numeric cleaning for relevant columns
    cols_to_make_numeric = (
        ["RunID", "IOPS", "IODEPTH", "NUMJOBS"]
        if (args.metric == "IOPS")
        else [
            col
            for col in df.columns
            if col not in ["BS", "DS", "RW_MIX", "RW_TYPE", "MODE"]
        ]
    )

    for col in cols_to_make_numeric:
        if col in df.columns:  # Check if column actually exists from CSV read
            df[col] = clean_numeric(df[col])

    print("\nDataFrame head after cleaning (first 2 rows):")
    print(df.head(2))
    return df


def verify_and_gen_iops_general_plots(df):
    metric = get_metric()

    if df.empty:
        print(
            "DataFrame is empty after loading and cleaning. No plots will be generated."
        )
        return

    df_m = df[df["MODE"] == metric].copy()

    if not ("IODEPTH" in df_m.columns and "NUMJOBS" in df_m.columns):
        print(
            "IODEPTH and/or NUMJOBS columns are missing. Skipping concurrent average performance bars."
        )
    else:
        unique_mixes_conc = df_m["RW_MIX"].unique()
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

        ds_names = cfg_parser.get_ds_names_from_cfg()

        plot_general_hist(
            df=df_m,
            nj=unique_nj[0],
            iodepth=unique_id[0],
            bs_val=unique_bss_conc[0],
            metric=metric,
            ds_values=ds_names,
            save_directory="plots/",
            filename=f"{metric}_general_hist_nj{unique_nj[0]}_id{unique_id[0]}",
        )


df = process_df()

verify_and_gen_iops_general_plots(df)
