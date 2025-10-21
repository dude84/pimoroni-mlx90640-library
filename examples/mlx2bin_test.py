#!/usr/bin/env python3
"""
MLX90640 Binary Frame Consumer

Reads binary frames from mlx2bin subprocess and displays them with:
- Real-time FPS calculation
- Optional ASCII inferno colormap rendering
"""

import subprocess
import struct
import time
import sys
import argparse
from collections import deque

# ANSI color codes for JET colormap
class Colors:
    RED = '\033[31m'
    GREEN = '\033[32m'
    YELLOW = '\033[33m'
    BLUE = '\033[34m'
    MAGENTA = '\033[35m'
    CYAN = '\033[36m'
    WHITE = '\033[37m'
    RESET = '\033[0m'

# Magic header: 0x4D, 0x4C, 0x58, 0xFF
FRAME_MAGIC = b'\x4D\x4C\x58\xFF'
FRAME_HEADER_SIZE = 4
TEMP_DATA_SIZE = 768 * 4  # 768 floats * 4 bytes each
FRAME_SIZE = FRAME_HEADER_SIZE + TEMP_DATA_SIZE

def get_color_for_temp(temp):
    """Get ANSI color for temperature value using inferno colormap"""
    if temp > 35.0:
        return Colors.WHITE      # White (hottest)
    elif temp > 32.0:
        return Colors.YELLOW      # Yellow
    elif temp > 29.0:
        return Colors.RED         # Red/Orange
    elif temp > 26.0:
        return Colors.MAGENTA     # Purple/Red
    elif temp > 23.0:
        return Colors.BLUE        # Dark purple/blue
    elif temp > 20.0:
        return Colors.CYAN        # Dark blue
    elif temp > 17.0:
        return Colors.GREEN       # Very dark blue/green
    else:
        return Colors.BLUE        # Black (coldest)

def render_ascii_frame(temperatures, show_ascii=True):
    """Render ASCII representation of thermal frame"""
    if not show_ascii:
        return
    
    # Clear screen and move cursor to top
    print('\033[2J\033[H', end='')
    
    # Render 24x32 grid (y=0 to 23, x=0 to 31)
    for y in range(24):
        for x in range(32):
            # Convert 2D coordinates to 1D index (same as C++ code)
            idx = 32 * (23 - y) + x
            temp = temperatures[idx]
            
            # Clamp temperature for display
            if temp > 99.99:
                temp = 99.99
            
            # Get color and render block
            color = get_color_for_temp(temp)
            print(f"{color}██{Colors.RESET}", end='')
        print()  # New line after each row

def calculate_fps(frame_times, window_size=30):
    """Calculate FPS using sliding window"""
    if len(frame_times) < 2:
        return 0.0
    
    # Use last window_size frames for FPS calculation
    recent_times = list(frame_times)[-window_size:]
    if len(recent_times) < 2:
        return 0.0
    
    time_diff = recent_times[-1] - recent_times[0]
    if time_diff <= 0:
        return 0.0
    
    return (len(recent_times) - 1) / time_diff

def parse_frame(data):
    """Parse binary frame data"""
    if len(data) != FRAME_SIZE:
        return None
    
    # Check magic header
    if data[:4] != FRAME_MAGIC:
        return None
    
    # Parse temperature data (768 floats)
    temp_data = data[FRAME_HEADER_SIZE:]
    temperatures = struct.unpack('768f', temp_data)
    
    return list(temperatures)

def main():
    parser = argparse.ArgumentParser(description='MLX90640 Binary Frame Consumer')
    parser.add_argument('--refresh-rate', type=int, default=16, help='MLX90640 refresh rate (0.5,1,2,4,8,16,32,64 [Hz], default: 16Hz)')
    parser.add_argument('--adc-resolution', type=int, default=19, help='ADC resolution in bits (16-19, default: 19)')
    parser.add_argument('--emissivity', type=float, default=0.98, help='Emissivity (0.0-1.0, default: 0.98)')
    parser.add_argument('--interleave', type=int, default=1, help='Interleave/chess mode (0/1, default: 1)')
    parser.add_argument('--bad-pixel-correction', type=int, default=1, help='Bad pixel correction (0/1, default: 1)')
    parser.add_argument('--stats-only', action='store_true', help='Show only FPS stats, no ASCII rendering')
    args = parser.parse_args()
    
    # Build mlx2bin command with parameters (no verbose/diagnostics to avoid breaking binary pipe)
    cmd = ['./mlx2bin']
    cmd.extend(['--refresh-rate', str(args.refresh_rate)])
    cmd.extend(['--adc-resolution', str(args.adc_resolution)])
    cmd.extend(['--emissivity', str(args.emissivity)])
    cmd.extend(['--interleave', str(args.interleave)])
    cmd.extend(['--bad-pixel-correction', str(args.bad_pixel_correction)])
    
    # Start mlx2bin subprocess
    try:
        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0  # Unbuffered for real-time data
        )
    except FileNotFoundError:
        print("Error: mlx2bin executable not found. Make sure it's compiled and in the current directory.")
        sys.exit(1)
    
    # Calculate actual FPS from refresh rate
    actual_fps = (args.refresh_rate == 0) and 1 or (1 << (args.refresh_rate - 1))
    
    # Display configuration
    print("MLX90640 Configuration:")
    print(f"  Refresh Rate: {args.refresh_rate} ({actual_fps}Hz)")
    print(f"  ADC Resolution: {args.adc_resolution} bits")
    print(f"  Emissivity: {args.emissivity}")
    print(f"  Interleave: {'ON' if args.interleave else 'OFF'}")
    print(f"  Bad Pixel Correction: {'ON' if args.bad_pixel_correction else 'OFF'}")
    print(f"  Display Mode: {'Stats Only' if args.stats_only else 'ASCII + Stats'}")
    print()
    print("Press Ctrl+C to stop")
    
    frame_times = deque(maxlen=100)  # Keep last 100 frame times
    frame_count = 0
    start_time = time.time()
    
    try:
        while True:
            # Read exactly one frame
            frame_data = b''
            while len(frame_data) < FRAME_SIZE:
                chunk = process.stdout.read(FRAME_SIZE - len(frame_data))
                if not chunk:
                    print("Error: Subprocess ended unexpectedly")
                    break
                frame_data += chunk
            
            if len(frame_data) != FRAME_SIZE:
                break
            
            # Parse frame
            temperatures = parse_frame(frame_data)
            if temperatures is None:
                print("Error: Invalid frame data")
                continue
            
            # Record timing
            current_time = time.time()
            frame_times.append(current_time)
            frame_count += 1
            
            # Calculate and display FPS
            fps = calculate_fps(frame_times)
            elapsed = current_time - start_time
            
            if args.stats_only:
                print(f"\rFrame: {frame_count:6d} | FPS: {fps:6.2f} | Elapsed: {elapsed:6.1f}s | Config: {args.refresh_rate}({actual_fps}Hz) | {args.adc_resolution}bit | ε={args.emissivity} | {'INT' if args.interleave else 'CHESS'} | {'BPC' if args.bad_pixel_correction else 'NO-BPC'}", end='', flush=True)
            else:
                # Render ASCII frame
                render_ascii_frame(temperatures, not args.stats_only)
                
                # Show stats
                print(f"Frame: {frame_count:6d} | FPS: {fps:6.2f} | Elapsed: {elapsed:6.1f}s")
                
                # Show temperature range
                min_temp = min(temperatures)
                max_temp = max(temperatures)
                print(f"Temp range: {min_temp:5.1f}°C - {max_temp:5.1f}°C")
                
                # Show configuration
                print(f"Config: {args.refresh_rate}({actual_fps}Hz) | {args.adc_resolution}bit | ε={args.emissivity} | {'INT' if args.interleave else 'CHESS'} | {'BPC' if args.bad_pixel_correction else 'NO-BPC'}")
    
    except KeyboardInterrupt:
        print("\nStopping...")
    
    finally:
        # Clean up
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
        
        # Final stats
        if frame_count > 0:
            total_time = time.time() - start_time
            avg_fps = frame_count / total_time
            print(f"\nFinal stats: {frame_count} frames in {total_time:.1f}s (avg {avg_fps:.2f} FPS)")

if __name__ == '__main__':
    main()
