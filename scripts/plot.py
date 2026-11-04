import matplotlib.pyplot as plt
import os

os.makedirs("../data", exist_ok=True)

# Средние по 3 запускам
labels = ["20 ms", "100 ms"]
completed = [86, 238]
expired   = [564, 412]
times     = [10511, 10568]
avg_wait  = [15.6, 78.8]

# График 1: Completed vs Expired
plt.figure(figsize=(8, 5))
plt.bar(labels, completed, label="Completed", color="#4CAF50")
plt.bar(labels, expired, bottom=completed, label="Expired", color="#F44336")
plt.ylabel("Число заявок")
plt.title("Вариант 21: Completed и Expired (STUDENT_ID=90)")
plt.legend()
plt.tight_layout()
plt.savefig("../data/graph1_completed_expired.png", dpi=150)
print("OK: graph1_completed_expired.png")

# График 2: среднее время ожидания
plt.figure(figsize=(8, 5))
plt.plot([20, 100], avg_wait, marker="o", linewidth=2, markersize=10, color="#2196F3")
plt.xlabel("maxWait, мс")
plt.ylabel("Среднее время ожидания (completed), мс")
plt.title("Вариант 21: avg wait vs maxWait")
plt.grid(True, alpha=0.3)
plt.tight_layout()
plt.savefig("../data/graph2_avg_wait.png", dpi=150)
print("OK: graph2_avg_wait.png")

plt.show()