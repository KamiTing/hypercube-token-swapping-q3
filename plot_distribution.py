import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


# ============================================================
# Basic settings
# ============================================================

CSV_PATH = "output/step_distribution.csv"

OUTPUT_TABLE = "output/step_distribution_summary.csv"
OUTPUT_BFS_FIG = "output/bfs_step_distribution.png"
OUTPUT_COMPARE_FIG = "output/method_step_distribution_compare.png"


# ============================================================
# Load CSV
# ============================================================

df = pd.read_csv(CSV_PATH)

# 確保 steps 是整數，方便作圖
df["steps"] = df["steps"].astype(int)

# 如果某些欄位不存在，就自動補 0
expected_columns = [
    "bfs_count",
    "astar_count",
    "beam_count",
    "batcher_count"
]

for col in expected_columns:
    if col not in df.columns:
        df[col] = 0

# 計算比例，方便表格觀察
total_states = df["bfs_count"].sum()

df["bfs_ratio"] = df["bfs_count"] / total_states
df["astar_ratio"] = df["astar_count"] / total_states
df["beam_ratio"] = df["beam_count"] / total_states
df["batcher_ratio"] = df["batcher_count"] / total_states


# ============================================================
# Print table
# ============================================================

print("========================================")
print("Step Distribution Table")
print("========================================")
print(df.to_string(index=False))

print("\nTotal states:", total_states)

# 輸出整理後表格
df.to_csv(OUTPUT_TABLE, index=False)
print(f"\nSummary table saved to: {OUTPUT_TABLE}")


# ============================================================
# Plot 1: BFS exact baseline distribution
# ============================================================

plt.figure(figsize=(10, 6))

plt.bar(
    df["steps"],
    df["bfs_count"],
    label="BFS true table"
)

plt.xlabel("Minimum number of swaps")
plt.ylabel("Number of permutations")
plt.title("Q3 Hypercube Minimum Swap Distribution, BFS True Table")
plt.xticks(df["steps"])
plt.grid(axis="y", alpha=0.3)
plt.legend()

plt.tight_layout()
plt.savefig(OUTPUT_BFS_FIG, dpi=300)
plt.close()

print(f"BFS distribution figure saved to: {OUTPUT_BFS_FIG}")


# ============================================================
# Plot 2: Method comparison distribution
# ============================================================

plt.figure(figsize=(12, 6))

bar_width = 0.2
x = df["steps"]

plt.bar(
    x - 1.5 * bar_width,
    df["bfs_count"],
    width=bar_width,
    label="BFS true table"
)

plt.bar(
    x - 0.5 * bar_width,
    df["astar_count"],
    width=bar_width,
    label="A*"
)

plt.bar(
    x + 0.5 * bar_width,
    df["beam_count"],
    width=bar_width,
    label="Beam Search"
)

plt.bar(
    x + 1.5 * bar_width,
    df["batcher_count"],
    width=bar_width,
    label="Batcher's merge sort"
)

plt.xlabel("Number of swaps / steps")
plt.ylabel("Number of permutations")
plt.title("Q3 Hypercube Step Distribution Comparison")
plt.xticks(df["steps"])
plt.grid(axis="y", alpha=0.3)
plt.legend()

plt.tight_layout()
plt.savefig(OUTPUT_COMPARE_FIG, dpi=300)
plt.close()

print(f"Method comparison figure saved to: {OUTPUT_COMPARE_FIG}")


# ============================================================
# Optional: print important statistics
# ============================================================

def weighted_average_steps(count_col):
    total = df[count_col].sum()

    if total == 0:
        return 0

    return (df["steps"] * df[count_col]).sum() / total


print("\n========================================")
print("Average Steps")
print("========================================")
print(f"BFS true table     : {weighted_average_steps('bfs_count'):.6f}")
print(f"A*                 : {weighted_average_steps('astar_count'):.6f}")
print(f"Beam Search        : {weighted_average_steps('beam_count'):.6f}")
print(f"Batcher's baseline : {weighted_average_steps('batcher_count'):.6f}")


print("\n========================================")
print("Maximum Step With Nonzero Count")
print("========================================")

for col, name in [
    ("bfs_count", "BFS true table"),
    ("astar_count", "A*"),
    ("beam_count", "Beam Search"),
    ("batcher_count", "Batcher's baseline")
]:
    nonzero = df[df[col] > 0]

    if len(nonzero) == 0:
        print(f"{name:20s}: no data")
    else:
        max_step = nonzero["steps"].max()
        print(f"{name:20s}: {max_step}")
