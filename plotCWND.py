import matplotlib.pyplot as plt

# Data containers
time = []
cwnd = []
ssthresh = []

# Read data from the CSV file
with open("CWND.csv", "r") as fp:
    fp.readline()  # Skip the header
    for line in fp:
        data = line.strip().split(",")
        time.append(float(data[0]))  # Time in seconds
        cwnd.append(float(data[1]))  # Congestion window size
        ssthresh.append(int(data[2]))  # Slow-start threshold

# Create the plot
fig, ax1 = plt.subplots()

# Plot the congestion window size
ax1.plot(time, cwnd, "g-", label="Window Size (cwnd)")
ax1.set_xlabel("Time (seconds)")
ax1.set_ylabel("Window Size (num. packets)", color="g")
ax1.tick_params(axis="y", labelcolor="g")
ax1.set_ylim([0, max(cwnd) + 10])  # Adjust Y-limit for better visibility

# Plot the slow-start threshold on a secondary Y-axis
ax2 = ax1.twinx()
ax2.step(time, ssthresh, "r--", label="Slow-Start Threshold (ssthresh)")
ax2.set_ylabel("ss_thresh (num. packets)", color="r")
ax2.tick_params(axis="y", labelcolor="r")
ax2.set_ylim([0, max(ssthresh) + 10])  # Adjust Y-limit for better visibility

# Add a legend
ax1.legend(loc="upper left")
ax2.legend(loc="upper right")

# Show and save the plot
plt.title("Congestion Window and Slow-Start Threshold Over Time")
plt.grid()
plt.savefig("cwnd_plot.pdf")
plt.show()

