import os
import pandas as pd
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "output"
CSV_PATH = os.path.join(OUT_DIR, "q4_random_benchmark.csv")

OUT_SUMMARY = os.path.join(OUT_DIR, "q4_random_summary.csv")
OUT_STEPS_HIST = os.path.join(OUT_DIR, "q4_steps_hist_compare.png")
OUT_TIME_BOX = os.path.join(OUT_DIR, "q4_time_boxplot.png")
OUT_GAP_HIST = os.path.join(OUT_DIR, "q4_gap_vs_batcher_hist.png")


def main():
    if not os.path.exists(CSV_PATH):
        raise FileNotFoundError(f"Missing input CSV: {CSV_PATH}")

    df = pd.read_csv(CSV_PATH)

    required = [
        "astar_steps",
        "strong_astar_steps",
        "beam_steps",
        "batcher_swaps",
        "batcher_ok",
        "astar_sec",
        "strong_astar_sec",
        "beam_sec",
        "batcher_sec",
    ]
    for c in required:
        if c not in df.columns:
            raise ValueError(f"Missing required column: {c}")

    os.makedirs(OUT_DIR, exist_ok=True)

    df["astar_ok"] = (df["astar_steps"] >= 0).astype(int)
    df["strong_astar_ok"] = (df["strong_astar_steps"] >= 0).astype(int)
    df["beam_ok"] = (df["beam_steps"] >= 0).astype(int)

    df["astar_gap_vs_batcher"] = df.apply(
        lambda r: (r["astar_steps"] - r["batcher_swaps"]) if r["astar_steps"] >= 0 and r["batcher_ok"] == 1 else None,
        axis=1,
    )
    df["strong_astar_gap_vs_batcher"] = df.apply(
        lambda r: (r["strong_astar_steps"] - r["batcher_swaps"]) if r["strong_astar_steps"] >= 0 and r["batcher_ok"] == 1 else None,
        axis=1,
    )
    df["beam_gap_vs_batcher"] = df.apply(
        lambda r: (r["beam_steps"] - r["batcher_swaps"]) if r["beam_steps"] >= 0 and r["batcher_ok"] == 1 else None,
        axis=1,
    )

    summary = {
        "samples": len(df),
        "astar_success_rate": df["astar_ok"].mean(),
        "strong_astar_success_rate": df["strong_astar_ok"].mean(),
        "beam_success_rate": df["beam_ok"].mean(),
        "batcher_success_rate": (df["batcher_ok"] == 1).mean(),
        "avg_astar_steps": df.loc[df["astar_steps"] >= 0, "astar_steps"].mean(),
        "avg_strong_astar_steps": df.loc[df["strong_astar_steps"] >= 0, "strong_astar_steps"].mean(),
        "avg_beam_steps": df.loc[df["beam_steps"] >= 0, "beam_steps"].mean(),
        "avg_batcher_swaps": df.loc[df["batcher_ok"] == 1, "batcher_swaps"].mean(),
        "avg_astar_sec": df["astar_sec"].mean(),
        "avg_strong_astar_sec": df["strong_astar_sec"].mean(),
        "avg_beam_sec": df["beam_sec"].mean(),
        "avg_batcher_sec": df["batcher_sec"].mean(),
        "avg_case_wall_sec": df["case_wall_sec"].mean() if "case_wall_sec" in df.columns else None,
        "avg_astar_expanded": df.loc[df["astar_steps"] >= 0, "astar_expanded"].mean() if "astar_expanded" in df.columns else None,
        "avg_strong_astar_expanded": df.loc[df["strong_astar_steps"] >= 0, "strong_astar_expanded"].mean() if "strong_astar_expanded" in df.columns else None,
        "strong_astar_same_steps_rate": (
            (df["astar_steps"] == df["strong_astar_steps"]) & (df["astar_steps"] >= 0)
        ).mean(),
        "avg_astar_gap_vs_batcher": df["astar_gap_vs_batcher"].dropna().mean(),
        "avg_strong_astar_gap_vs_batcher": df["strong_astar_gap_vs_batcher"].dropna().mean(),
        "avg_beam_gap_vs_batcher": df["beam_gap_vs_batcher"].dropna().mean(),
    }

    pd.DataFrame([summary]).to_csv(OUT_SUMMARY, index=False)

    plt.figure(figsize=(11, 6))
    astar_ok = df.loc[df["astar_steps"] >= 0, "astar_steps"]
    strong_astar_ok = df.loc[df["strong_astar_steps"] >= 0, "strong_astar_steps"]
    beam_ok = df.loc[df["beam_steps"] >= 0, "beam_steps"]
    batcher_ok = df.loc[df["batcher_ok"] == 1, "batcher_swaps"]

    min_step = int(min(astar_ok.min() if len(astar_ok) else 0,
                       strong_astar_ok.min() if len(strong_astar_ok) else 0,
                       beam_ok.min() if len(beam_ok) else 0,
                       batcher_ok.min() if len(batcher_ok) else 0))
    max_step = int(max(astar_ok.max() if len(astar_ok) else 0,
                       strong_astar_ok.max() if len(strong_astar_ok) else 0,
                       beam_ok.max() if len(beam_ok) else 0,
                       batcher_ok.max() if len(batcher_ok) else 0))

    steps = list(range(min_step, max_step + 1))
    astar_counts = astar_ok.value_counts().reindex(steps, fill_value=0)
    strong_astar_counts = strong_astar_ok.value_counts().reindex(steps, fill_value=0)
    beam_counts = beam_ok.value_counts().reindex(steps, fill_value=0)
    batcher_counts = batcher_ok.value_counts().reindex(steps, fill_value=0)

    bar_w = 0.20
    x = steps
    x_astar = [v - 1.5 * bar_w for v in x]
    x_strong_astar = [v - 0.5 * bar_w for v in x]
    x_beam = [v + 0.5 * bar_w for v in x]
    x_batcher = [v + 1.5 * bar_w for v in x]

    plt.bar(x_astar, astar_counts.values, width=bar_w, label="Basic A* steps")
    plt.bar(x_strong_astar, strong_astar_counts.values, width=bar_w, label="Strong A* steps")
    plt.bar(x_beam, beam_counts.values, width=bar_w, label="Beam steps")
    plt.bar(x_batcher, batcher_counts.values, width=bar_w, label="Batcher swaps")

    plt.title("Q4 Random Test: Step/Swap Distribution")
    plt.xlabel("Steps / Swaps")
    plt.ylabel("Count")
    plt.xticks(steps)
    plt.grid(axis="y", alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(OUT_STEPS_HIST, dpi=300)
    plt.close()

    plt.figure(figsize=(9, 6))
    data = [df["astar_sec"], df["strong_astar_sec"], df["beam_sec"], df["batcher_sec"]]
    plt.boxplot(data, tick_labels=["Basic A*", "Strong A*", "Beam", "Batcher"], showfliers=False)
    plt.title("Q4 Random Test: Runtime Distribution")
    plt.ylabel("Seconds")
    plt.grid(axis="y", alpha=0.25)
    plt.tight_layout()
    plt.savefig(OUT_TIME_BOX, dpi=300)
    plt.close()

    plt.figure(figsize=(10, 6))
    astar_gap = df["astar_gap_vs_batcher"].dropna()
    strong_astar_gap = df["strong_astar_gap_vs_batcher"].dropna()
    beam_gap = df["beam_gap_vs_batcher"].dropna()

    if len(astar_gap) > 0:
        plt.hist(astar_gap, bins=30, alpha=0.5, label="Basic A* - Batcher")
    if len(strong_astar_gap) > 0:
        plt.hist(strong_astar_gap, bins=30, alpha=0.5, label="Strong A* - Batcher")
    if len(beam_gap) > 0:
        plt.hist(beam_gap, bins=30, alpha=0.5, label="Beam - Batcher")

    plt.axvline(0, color="black", linewidth=1.0, linestyle="--")
    plt.title("Q4 Random Test: Gap to Batcher Swaps")
    plt.xlabel("Gap (method steps - batcher swaps)")
    plt.ylabel("Count")
    plt.grid(axis="y", alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(OUT_GAP_HIST, dpi=300)
    plt.close()

    print("Saved:")
    print(f"- {OUT_SUMMARY}")
    print(f"- {OUT_STEPS_HIST}")
    print(f"- {OUT_TIME_BOX}")
    print(f"- {OUT_GAP_HIST}")


if __name__ == "__main__":
    main()
