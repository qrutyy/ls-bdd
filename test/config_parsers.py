import re

CONFIG_FILE_PATH = "configurable_params.sh"


def load_ds_mapping():
    """
    Read configurable_params.sh and parse PY_PL_NEW_DS into a dictionary:
    PY_PL_NEW_DS="ht:Hash-table, sl:Skiplist, bt:B+ tree, rb:Red-Black tree"
    -> {'ht': 'Hash-table', 'sl': 'Skiplist', ...}
    """
    mapping = {}
    try:
        with open(CONFIG_FILE_PATH, "r") as f:
            for line in f:
                if "PY_PL_NEW_DS" in line:
                    line = line.lstrip("#").strip()
                    match = re.search(r'=\s*["\']?([^"\']+)["\']?', line)
                    if match:
                        items = match.group(1).split(",")
                        for item in items:
                            if ":" in item:
                                key, value = item.split(":", 1)
                                mapping[key.strip()] = value.strip()
    except FileNotFoundError:
        pass
    return mapping


def get_ds_names_from_cfg():
    """
    Open a shell config file, parse the line defining PY_PL_DS_TO_PLOT,
    and return a list of IDs.
    """
    with open(CONFIG_FILE_PATH, "r") as f:
        for line in f:
            if "PY_PL_DS_TO_PLOT" in line:
                line = line.lstrip("#").strip()

                match = re.search(r'=\s*["\']?([^"\']+)["\']?', line)
                if match:
                    ids = [x.strip() for x in match.group(1).split(",") if x.strip()]
                    return ids
    return []


def parse_plot_titles():
    """
    Parse only the PL_GENERAL_* title lines from the config file.
    Returns a dict of variable → value.
    """

    vars_of_interest = [
        "PL_GENERAL_IOPS_Y_TITLE",
        "PL_GENERAL_LAT_Y_TITLE",
        "PL_GENERAL_IOPS_TITLE_TEMPLATE",
        "PL_GENERAL_LAT_TITLE_TEMPLATE",
        "PL_IOPS_CONC_X_TITLE",
        "PL_IOPS_CONC_Y_TITLE",
        "PL_IOPS_CONC_TITLE_TEMPLATE",
    ]

    # Pre-build regex for matching any of the variables
    pattern = re.compile(
        r"^#?\s*(" + "|".join(vars_of_interest) + r')\s*=\s*["\']?(.*?)["\']?\s*$'
    )

    results = {}

    with open(CONFIG_FILE_PATH, "r") as f:
        for line in f:
            line = line.strip()
            m = pattern.match(line)
            if m:
                var, value = m.groups()
                results[var] = value

    return results
