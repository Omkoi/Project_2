import matplotlib.pyplot as plt
import pandas as pd
from argparse import ArgumentParser

def plot_cwnd(cwnd_file, output_dir):
    # Read the CWND.csv file
    df = pd.read_csv(cwnd_file)
    
    # Create the plot
    plt.figure(figsize=(12, 6), facecolor='w')
    
    # Plot CWND
    plt.plot(df['Time'] - df['Time'].iloc[0], df['CWND'], 
             label='CWND', color='blue', linewidth=2)
    
    # Plot ssthresh
    plt.plot(df['Time'] - df['Time'].iloc[0], df['ssthresh'], 
             label='ssthresh', color='red', linestyle='--', linewidth=2)
    
    # Customize the plot
    plt.grid(True, which="both", linestyle='--', alpha=0.7)
    plt.xlabel('Time (s)')
    plt.ylabel('Window Size (segments)')
    plt.title('Congestion Window (CWND) and Slow Start Threshold Evolution')
    plt.legend()
    
    # Save the plot
    plt.savefig(f'{output_dir}/cwnd_evolution.pdf', 
                dpi=300, bbox_inches='tight')
    plt.close()

def main():
    parser = ArgumentParser(description="Plot CWND evolution")
    
    parser.add_argument('--cwnd-file', '-c',
                        help="Path to CWND.csv file",
                        default="CWND.csv")
    
    parser.add_argument('--output-dir', '-o',
                        help="Directory to store output plot",
                        default=".")
    
    args = parser.parse_args()
    
    plot_cwnd(args.cwnd_file, args.output_dir)

if __name__ == "__main__":
    main() 