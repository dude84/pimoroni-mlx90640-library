#include <stdint.h>
#include <iostream>
#include <cstring>
#include <fstream>
#include <chrono>
#include <thread>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <algorithm>
#include "headers/MLX90640_API.h"

#define MLX_I2C_ADDR 0x33

// Default configuration values
#define DEFAULT_EMISSIVITY 0.98f
#define DEFAULT_REFRESH_RATE 5  // 32Hz (index 5 in refreshRateNames)
#define DEFAULT_ADC_RESOLUTION 3  // 19bit (index 3 in resolutionNames)
#define DEFAULT_INTERLEAVE 1
#define DEFAULT_BAD_PIXEL_CORRECTION 1

// Valid refresh rates: 0.5Hz, 1Hz, 2Hz, 4Hz, 8Hz, 16Hz, 32Hz, 64Hz
static const float refreshRateHz[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f};

static const char* resolutionNames[] = {
    "16bit",
    "17bit", 
    "18bit",
    "19bit"
};

// Despite the refresh rate being ostensibly X Hz
// The frame is often not ready in time
// This offset is added to the FRAME_TIME_MICROS
// to account for this.
#define OFFSET_MICROS 850

#define FRAME_HEADER_SIZE 4
#define TEMP_DATA_SIZE 768 * sizeof(float)
#define FRAME_SIZE (FRAME_HEADER_SIZE + TEMP_DATA_SIZE)

// Magic header for each frame: 0x4D, 0x4C, 0x58, 0xFF
static const uint8_t FRAME_MAGIC[4] = {0x4D, 0x4C, 0x58, 0xFF};

// Helper function to convert Hz to refresh rate index
int hzToRefreshRateIndex(float hz) {
    for(int i = 0; i < 8; i++) {
        if(fabs(refreshRateHz[i] - hz) < 0.1f) {
            return i;
        }
    }
    return -1; // Invalid Hz value
}

void print_help() {
    printf("MLX90640 Binary Frame Output\n");
    printf("Usage: mlx2bin [options]\n\n");
    printf("Options:\n");
    printf("  -h, --help                    Show this help message\n");
    printf("  -d, --diagnostics             Run diagnostic test\n");
    printf("  -s, --speed-test              Run speed test (measure FPS without binary output)\n");
    printf("  -v, --verbose                 Show configuration parameters\n");
    printf("  -e, --emissivity <value>      Emissivity (0.0-1.0, default: %.2f)\n", DEFAULT_EMISSIVITY);
    printf("  -r, --refresh-rate <hz>        Refresh rate in Hz (0.5, 1, 2, 4, 8, 16, 32, 64, default: %.1fHz)\n", refreshRateHz[DEFAULT_REFRESH_RATE]);
    printf("  -a, --adc-resolution <bits>   ADC resolution in bits (16-19, default: %s)\n", resolutionNames[DEFAULT_ADC_RESOLUTION]);
    printf("  -i, --interleave <0|1>        Interleave/chess mode (default: %d)\n", DEFAULT_INTERLEAVE);
    printf("  -b, --bad-pixel-correction <0|1> Bad pixel correction (default: %d)\n", DEFAULT_BAD_PIXEL_CORRECTION);
    printf("\nRefresh rates: ");
    for(int i = 0; i < 8; i++) {
        printf("%.1fHz ", refreshRateHz[i]);
    }
    printf("\nADC resolutions: 16bit 17bit 18bit 19bit\n");
}

int main(int argc, char *argv[]){
    // Default values
    float emissivity = DEFAULT_EMISSIVITY;
    int refresh_rate = DEFAULT_REFRESH_RATE;
    int adc_resolution = DEFAULT_ADC_RESOLUTION;
    int interleave = DEFAULT_INTERLEAVE;
    int bad_pixel_correction = DEFAULT_BAD_PIXEL_CORRECTION;
    int test_mode = 0;
    int speed_test_mode = 0;
    bool verbose = false;

    // Parse command line arguments
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        else if(strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--diagnostics") == 0) {
            test_mode = 1;
        }
        else if(strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--speed-test") == 0) {
            speed_test_mode = 1;
        }
        else if(strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        }
        else if(strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--emissivity") == 0) {
            if(i + 1 < argc) {
                emissivity = atof(argv[++i]);
                if(emissivity < 0.0f || emissivity > 1.0f) {
                    fprintf(stderr, "Error: Emissivity must be between 0.0 and 1.0\n");
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: --emissivity requires a value\n");
                return 1;
            }
        }
        else if(strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--refresh-rate") == 0) {
            if(i + 1 < argc) {
                float hz = atof(argv[++i]);
                refresh_rate = hzToRefreshRateIndex(hz);
                if(refresh_rate == -1) {
                    fprintf(stderr, "Error: Invalid refresh rate %.1fHz. Valid values: ", hz);
                    for(int j = 0; j < 8; j++) {
                        fprintf(stderr, "%.1fHz ", refreshRateHz[j]);
                    }
                    fprintf(stderr, "\n");
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: --refresh-rate requires a value\n");
                return 1;
            }
        }
        else if(strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--adc-resolution") == 0) {
            if(i + 1 < argc) {
                int bits = atoi(argv[++i]);
                switch(bits) {
                    case 16: adc_resolution = 0; break;
                    case 17: adc_resolution = 1; break;
                    case 18: adc_resolution = 2; break;
                    case 19: adc_resolution = 3; break;
                    default:
                        fprintf(stderr, "Error: ADC resolution must be 16, 17, 18, or 19 bits\n");
                        return 1;
                }
            } else {
                fprintf(stderr, "Error: --adc-resolution requires a value\n");
                return 1;
            }
        }
        else if(strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interleave") == 0) {
            if(i + 1 < argc) {
                interleave = atoi(argv[++i]);
                if(interleave != 0 && interleave != 1) {
                    fprintf(stderr, "Error: Interleave must be 0 or 1\n");
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: --interleave requires a value\n");
                return 1;
            }
        }
        else if(strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bad-pixel-correction") == 0) {
            if(i + 1 < argc) {
                bad_pixel_correction = atoi(argv[++i]);
                if(bad_pixel_correction != 0 && bad_pixel_correction != 1) {
                    fprintf(stderr, "Error: Bad pixel correction must be 0 or 1\n");
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: --bad-pixel-correction requires a value\n");
                return 1;
            }
        }
        else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_help();
            return 1;
        }
    }

    static uint16_t eeMLX90640[832];
    uint16_t frame[834];
    static float mlx90640To[768];
    float eTa;
    
    // Calculate frame time based on refresh rate
    // Note: MLX90640 needs 2 subframes for full frame, so actual FPS is half the refresh rate
    int refresh_hz = (refresh_rate == 0) ? 1 : (1 << (refresh_rate - 1)); // Convert index to Hz
    int actual_fps = refresh_hz / 2; // Actual FPS is half the refresh rate
    static long frame_time_micros = 1000000 / actual_fps;

    auto frame_time = std::chrono::microseconds(frame_time_micros + OFFSET_MICROS);

    // Print configuration (only if verbose)
    if(verbose) {
        printf("MLX90640 Configuration:\n");
        printf("  Emissivity: %.2f\n", emissivity);
        printf("  Refresh Rate: %.1fHz (index %d)\n", refreshRateHz[refresh_rate], refresh_rate);
        printf("  Expected FPS: %d\n", actual_fps);
        printf("  ADC Resolution: %s (index %d)\n", resolutionNames[adc_resolution], adc_resolution);
        printf("  Interleave: %s\n", interleave ? "ON" : "OFF");
        printf("  Bad Pixel Correction: %s\n", bad_pixel_correction ? "ON" : "OFF");
        printf("  Actual FPS: %d\n", actual_fps);
        printf("\n");
    }

    MLX90640_SetDeviceMode(MLX_I2C_ADDR, 0);
    MLX90640_SetSubPageRepeat(MLX_I2C_ADDR, 0);
    
    // Set refresh rate
    MLX90640_SetRefreshRate(MLX_I2C_ADDR, refresh_rate);
    
    // Set interleave/chess mode
    if(interleave) {
        MLX90640_SetChessMode(MLX_I2C_ADDR);
    } else {
        MLX90640_SetInterleavedMode(MLX_I2C_ADDR);
    }

    paramsMLX90640 mlx90640;
    MLX90640_DumpEE(MLX_I2C_ADDR, eeMLX90640);
    MLX90640_SetResolution(MLX_I2C_ADDR, adc_resolution);
    MLX90640_ExtractParameters(eeMLX90640, &mlx90640);

    if(test_mode) {
        printf("=== DIAGNOSTIC TEST MODE ===\n");
        printf("EEPROM Dump completed\n");
        printf("Extracted parameters:\n");
        printf("  KsTa: %.6f\n", mlx90640.KsTa);
        printf("  ksTo[0]: %.6f\n", mlx90640.ksTo[0]);
        printf("  Alpha scale: %d\n", mlx90640.alphaScale);
        printf("  Kta scale: %d\n", mlx90640.ktaScale);
        printf("  Kv scale: %d\n", mlx90640.kvScale);
        printf("  Resolution EE: %d\n", mlx90640.resolutionEE);
        printf("  Calibration mode: %d\n", mlx90640.calibrationModeEE);
        printf("\nTesting frame capture...\n");
        
        // Capture one test frame
        MLX90640_GetFrameData(MLX_I2C_ADDR, frame);
        eTa = MLX90640_GetTa(frame, &mlx90640);
        MLX90640_CalculateTo(frame, &mlx90640, emissivity, eTa, mlx90640To);
        
        if(bad_pixel_correction) {
            MLX90640_BadPixelsCorrection(mlx90640.brokenPixels, mlx90640To, 1, &mlx90640);
            MLX90640_BadPixelsCorrection(mlx90640.outlierPixels, mlx90640To, 1, &mlx90640);
        }
        
        printf("Test frame captured successfully!\n");
        printf("Ambient temperature: %.2f°C\n", eTa);
        printf("Temperature range: %.2f°C - %.2f°C\n", 
               *std::min_element(mlx90640To, mlx90640To + 768),
               *std::max_element(mlx90640To, mlx90640To + 768));
        printf("Diagnostic test completed.\n");
        return 0;
    }

    if(speed_test_mode) {
        printf("=== SPEED TEST MODE ===\n");
        printf("Configuration:\n");
        printf("  Refresh Rate: %.1fHz (index %d)\n", refreshRateHz[refresh_rate], refresh_rate);
        printf("  Expected FPS: %d\n", actual_fps);
        printf("  ADC Resolution: %s (index %d)\n", resolutionNames[adc_resolution], adc_resolution);
        printf("  Interleave: %s\n", interleave ? "ON" : "OFF");
        printf("  Bad Pixel Correction: %s\n", bad_pixel_correction ? "ON" : "OFF");
        printf("  Expected FPS: %d\n", actual_fps);
        printf("\nMeasuring actual FPS...\n");
        printf("Press Ctrl+C to stop\n\n");
        
        int frame_count = 0;
        auto start_time = std::chrono::system_clock::now();
        auto last_report_time = start_time;
        
        while (1) {
            auto frame_start = std::chrono::system_clock::now();
            MLX90640_GetFrameData(MLX_I2C_ADDR, frame);
            MLX90640_InterpolateOutliers(frame, eeMLX90640);

            eTa = MLX90640_GetTa(frame, &mlx90640);
            MLX90640_CalculateTo(frame, &mlx90640, emissivity, eTa, mlx90640To);

            if(bad_pixel_correction) {
                MLX90640_BadPixelsCorrection(mlx90640.brokenPixels, mlx90640To, 1, &mlx90640);
                MLX90640_BadPixelsCorrection(mlx90640.outlierPixels, mlx90640To, 1, &mlx90640);
            }
            
            frame_count++;
            auto frame_end = std::chrono::system_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - start_time);
            auto report_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - last_report_time);
            
            // Report FPS every 1 second
            if(report_elapsed.count() >= 1000) {
                double actual_fps = (double)frame_count * 1000000.0 / elapsed.count();
                printf("\rFrames: %6d | Elapsed: %6.1fs | Actual FPS: %6.2f", 
                       frame_count, elapsed.count() / 1000000.0, actual_fps, actual_fps);
                fflush(stdout);
                last_report_time = frame_end;
            }
            
            // Sleep to maintain timing
            auto frame_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start);
            std::this_thread::sleep_for(std::chrono::microseconds(frame_time - frame_elapsed));
        }
        
        return 0;
    }

    while (1){
        auto start = std::chrono::system_clock::now();
        MLX90640_GetFrameData(MLX_I2C_ADDR, frame);
        MLX90640_InterpolateOutliers(frame, eeMLX90640);

        eTa = MLX90640_GetTa(frame, &mlx90640); // Sensor ambient temperature
        MLX90640_CalculateTo(frame, &mlx90640, emissivity, eTa, mlx90640To); // Calculate temperature of all pixels

        // Apply bad pixel correction if enabled
        if(bad_pixel_correction) {
            MLX90640_BadPixelsCorrection(mlx90640.brokenPixels, mlx90640To, 1, &mlx90640);
            MLX90640_BadPixelsCorrection(mlx90640.outlierPixels, mlx90640To, 1, &mlx90640);
        }

        // Write magic header to stdout
        fwrite(FRAME_MAGIC, 1, FRAME_HEADER_SIZE, stdout);
        
        // Write raw temperature data to stdout
        fwrite(mlx90640To, sizeof(float), 768, stdout);
        fflush(stdout); // push to stdout now

        auto end = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        std::this_thread::sleep_for(std::chrono::microseconds(frame_time - elapsed));
    }

    return 0;
}
