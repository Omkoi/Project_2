import pandas as pd
import matplotlib.pyplot as plt

# Load throughput data
file_path = 'throughput.csv'  # Replace with the actual file path
throughput_data = pd.read_csv(file_path, names=["time", "bytes"])

# Convert 'time' column to float
throughput_data["time"] = pd.to_numeric(throughput_data["time"], errors="coerce")
throughput_data["bytes"] = pd.to_numeric(throughput_data["bytes"], errors="coerce")

# Drop any rows with invalid data
throughput_data.dropna(inplace=True)

# Calculate throughput (in Mbps) for 1-second intervals
time_intervals = []
throughput = []

start_time = throughput_data["time"].iloc[0]
bytes_in_interval = 0
interval_duration = 1.0  # 1 second

for index, row in throughput_data.iterrows():
    current_time = row["time"]
    bytes_ = row["bytes"]
    if current_time - start_time <= interval_duration:
        bytes_in_interval += bytes_
    else:
        throughput.append(bytes_in_interval * 8 / 1e6)  # Convert bytes to Mbps
        time_intervals.append(start_time)
        start_time += interval_duration
        bytes_in_interval = bytes_

# Add final interval if data exists
if bytes_in_interval > 0:
    throughput.append(bytes_in_interval * 8 / 1e6)
    time_intervals.append(start_time)

# Plot the throughput graph
plt.figure(figsize=(10, 5))
plt.plot(time_intervals, throughput, lw=2, color='r', label='Throughput (Mbps)')
plt.xlabel("Time (s)")
plt.ylabel("Throughput (Mbps)")
plt.title("Throughput Over Time")
plt.grid(True, which="both")
plt.legend()
plt.tight_layout()
plt.savefig("throughput.pdf", dpi=1000, bbox_inches='tight')
plt.show()


