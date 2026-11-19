import csv
import os
from collections import defaultdict
from statistics import mean

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DATA = os.path.join(BASE, "data", "results.csv")
OUT = os.path.join(BASE, "report")
os.makedirs(OUT, exist_ok=True)

with open(DATA, newline="", encoding="utf-8") as f:
    rows = list(csv.DictReader(f))

groups = defaultdict(list)
for r in rows:
    if not r["label"].startswith("sweep"):
        groups[r["label"]].append(r)

summary_path = os.path.join(BASE, "data", "summary.csv")
with open(summary_path, "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["label", "run1_ms", "run2_ms", "run3_ms", "mean_ms", "created", "processed", "dropped",
                "loss_pct", "avg_queue_wait_ms", "max_queue_len", "blocked", "avg_blocked_ms", "violations"])
    for label, rs in sorted(groups.items()):
        times = [float(r["total_ms"]) for r in rs]
        t = (times + ["", "", ""])[:3]
        w.writerow([label] + t + [
            round(mean(times), 1),
            round(mean(float(r["created"]) for r in rs), 1),
            round(mean(float(r["processed"]) for r in rs), 1),
            round(mean(float(r["dropped"]) for r in rs), 1),
            round(mean(float(r["loss_pct"]) for r in rs), 2),
            round(mean(float(r["avg_queue_wait_ms"]) for r in rs), 2),
            round(mean(float(r["max_queue_len"]) for r in rs), 1),
            round(mean(float(r["blocked"]) for r in rs), 1),
            round(mean(float(r["avg_blocked_ms"]) for r in rs), 2),
            sum(int(r["violations"]) for r in rs),
        ])
print("Сводная таблица записана:", summary_path)
with open(summary_path, encoding="utf-8") as f:
    print(f.read())

sweep = defaultdict(list)
for r in rows:
    if r["label"].startswith("sweep"):
        rate = int(r["gen"]) * 1000.0 / int(r["interval"])
        sweep[(r["mode"], rate)].append(r)

def series(mode, field):
    pts = sorted((rate, mean(float(r[field]) for r in rs))
                 for (m, rate), rs in sweep.items() if m == mode)
    return [p[0] for p in pts], [p[1] for p in pts]

def make_plot(field, ylabel, title, filename, capacity=None):
    plt.figure(figsize=(7, 4.5))
    for mode, name in (("wait", "режим ожидания"), ("drop", "режим отбрасывания")):
        x, y = series(mode, field)
        plt.plot(x, y, marker="o", label=name)
    if capacity:
        plt.axvline(capacity, color="gray", linestyle="--", label="пропускная способность обработчиков")
    plt.xscale("log")
    plt.xlabel("Интенсивность поступления заявок (номинальная), заявок/с")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, which="both", alpha=0.3)
    plt.legend()
    plt.tight_layout()
    path = os.path.join(OUT, filename)
    plt.savefig(path, dpi=150)
    plt.close()
    print("График:", path)

CAPACITY = 2 * 1000.0 / 5
make_plot("throughput", "Пропускная способность, обработано заявок/с",
          "Зависимость пропускной способности от интенсивности", "fig1_throughput.png", CAPACITY)
make_plot("loss_pct", "Потерянные заявки, %",
          "Зависимость доли потерянных заявок от интенсивности", "fig2_loss.png", CAPACITY)
make_plot("avg_queue_wait_ms", "Среднее время заявки в очереди, мс",
          "Зависимость времени ожидания в очереди от интенсивности", "fig3_queue_wait.png", CAPACITY)
