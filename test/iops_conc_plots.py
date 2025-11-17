import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os
import argparse
import re
import config_parsers as cfg_parser

RESULTS_FILE = "logs/fio_results.dat"

DS_COLORS = {"sl": "steelblue", "ht": "indianred", "rb": "seagreen", "bt": "darkkhaki"}

DEFAULT_DS_MAPPING = {
    "ht": "Hash-table",
    "sl": "Skiplist",
    "bt": "B+ tree",
    "rb": "Red-Black tree",
}

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


def get_formatted_rw_type(rw):
    match (rw):
        case "rw":
            return "sequential"
        case "randrw":
            return "random"
        case _:
            print(
                "Unknown rw type, check get_formatted_rw_type function in iops_conc_plots.py"
            )
            return "unknown"


def plot_conc_iops_hist(
    df,
    bs_val,
    mix_val,
    rw_type,
    ds_values=None,
    save_directory_base="iops_conc",
    filename_prefix_detail="",
    is_rewrite=False,
):
    """
    Bar-chart: median IOPS vs (NUMJOBS, IODEPTH) for specified BS и MIX.
    Each group consists of one plot per ds.
    """

    subset = df[(df["BS"] == bs_val) & (df["MIX"] == mix_val)]
    if subset.empty:
        print(f"[skip] No data for BS={bs_val}, MIX={mix_val}")
        return

    if ds_values is None:
        ds_values = sorted(subset["ds"].unique())

    grouped = (
        subset.groupby(["NUMJOBS", "IODEPTH", "DS"])["IOPS"].median().reset_index()
    )

    x_groups = sorted(grouped[["NUMJOBS", "IODEPTH"]].drop_duplicates().values.tolist())

    values_per_ds = {ds: [] for ds in ds_values}
    labels = []
    for nj, iod in x_groups:
        labels.append(f"NJ={nj}, ID={iod}")
        for ds in ds_values:
            row = grouped[
                (grouped["NUMJOBS"] == nj)
                & (grouped["IODEPTH"] == iod)
                & (grouped["DS"] == ds)
            ]
            values_per_ds[ds].append(row["IOPS"].iloc[0] if not row.empty else 0)

    x = np.arange(len(labels))
    width = 0.8 / len(ds_values)

    plt.figure(figsize=(12, 7))
    for i, ds in enumerate(ds_values):
        plt.bar(
            x + (i - len(ds_values) / 2) * width + width / 2,
            values_per_ds[ds],
            width,
            color=DS_COLORS.get(ds, None),
            label=f"{get_formatted_ds_name(ds)}",
        )

    plt.xticks(x, labels, rotation=45, ha="right")
    plt.ylabel(PLOT_TITLES["PL_IOPS_CONC_Y_TITLE"])
    plt.xlabel(PLOT_TITLES["PL_IOPS_CONC_X_TITLE"])
    op = "write operations" if (mix_val == "0-100") else "read operations"
    tmplt = PLOT_TITLES["PL_IOPS_CONC_TITLE_TEMPLATE"]
    plt.title(f"{tmplt}\nBS={bs_val}K while {get_formatted_rw_type(rw_type)} {op}")

    plt.legend()
    plt.tight_layout()

    os.makedirs(save_directory_base, exist_ok=True)
    save_path = os.path.join(save_directory_base, f"{filename_prefix_detail}.png")
    plt.savefig(save_path)
    plt.close()

    print(f"[ok] Saved plot: {save_path}")


def verify_and_gen_iops_conc_plots(df):
    if df.empty:
        print(
            "DataFrame is empty after loading and cleaning. No plots will be generated."
        )
        return

    df_iops = df[df["MODE"] == "IOPS"].copy()

    if not ("IODEPTH" in df_iops.columns and "NUMJOBS" in df_iops.columns):
        print(
            "IODEPTH and/or NUMJOBS columns are missing. Skipping concurrent average performance bars."
        )
    else:
        unique_bss_conc = sorted(
            list(df_iops["BS"].unique()),
            key=parse_block_size_for_sorting,
        )
        unique_mixes_conc = df_iops["MIX"].unique()
        unique_mix_types_conc = df_iops["RW_TYPE"].unique()
        print(df_iops)
        assert len(unique_mixes_conc) == 1
        assert len(unique_mix_types_conc) == 1
        assert len(unique_bss_conc) == 1

        bs_val_c = unique_bss_conc[0]
        mix_val_c = unique_mixes_conc[0]
        rw_type_c = unique_mix_types_conc[0]
        subset_bs_mix_c = df_iops[
            (df_iops["BS"] == bs_val_c)
            & (df_iops["MIX"] == mix_val_c)
            & (df_iops["RW_TYPE"] == rw_type_c)
        ]
        if not subset_bs_mix_c.empty:
            plot_conc_iops_hist(
                df=subset_bs_mix_c,
                bs_val=bs_val_c,
                mix_val=mix_val_c,
                rw_type=rw_type_c,
                ds_values=["ht", "sl"],  # TODO: pass from plots.sh
                save_directory_base="plots/iops",
                filename_prefix_detail=f"iops_avg_bars_idnj_{mix_val_c}_{bs_val_c}_{rw_type_c}",
            )


def process_df():
    df = pd.read_csv(
        RESULTS_FILE,
        sep=r"\s+",
        skiprows=0,
        # Ensure all potential columns are named for robust parsing
        names=[
            "RunID",
            "DS",
            "BS",
            "MIX",
            "BW",
            "IOPS",
            "MODE",
            "RW_TYPE",
            "IODEPTH",
            "NUMJOBS",
        ],
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
    return df


df = process_df()
verify_and_gen_iops_conc_plots(df)
