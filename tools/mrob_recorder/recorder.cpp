// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "recorder.h"
#include <ctime>
#include <chrono>
#include <atomic>
#include <iostream>
#include <algorithm>

#include <k4a/k4a.h>
#include <k4arecord/record.h>

#include <stdio.h>

// #include <librealsense2/rs.hpp>

#include <opencv2/opencv.hpp>

#include <fstream>

using namespace std::chrono;
using namespace std;

inline static uint32_t k4a_convert_fps_to_uint(k4a_fps_t fps)
{
    uint32_t fps_int;
    switch (fps)
    {
    case K4A_FRAMES_PER_SECOND_5:
        fps_int = 5;
        break;
    case K4A_FRAMES_PER_SECOND_15:
        fps_int = 15;
        break;
    case K4A_FRAMES_PER_SECOND_30:
        fps_int = 30;
        break;
    default:
        fps_int = 0;
        break;
    }
    return fps_int;
}

// call k4a_device_close on every failed CHECK
#define CHECK(x, device)                                                                                               \
    {                                                                                                                  \
        auto retval = (x);                                                                                             \
        if (retval)                                                                                                    \
        {                                                                                                              \
            std::cerr << "Runtime error: " << #x << " returned " << retval << std::endl;                               \
            k4a_device_close(device);                                                                                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    }

std::atomic_bool exiting(false);

int do_recording(uint8_t device_index,
                 char *recording_filename,
                 int recording_length,
                 k4a_device_configuration_t *device_config,
                 bool record_imu,
                 int32_t absoluteExposureValue,
                 int32_t gain,
                 char *timestamps_table_filename)
{
    seconds recording_length_seconds(recording_length);
    const uint32_t installed_devices = k4a_device_get_installed_count();
    if (device_index >= installed_devices)
    {
        std::cerr << "Device not found." << std::endl;
        return 1;
    }

    k4a_device_t device;
    if (K4A_FAILED(k4a_device_open(device_index, &device)))
    {
        std::cerr << "Runtime error: k4a_device_open() failed " << std::endl;
    }

    char serial_number_buffer[256];
    size_t serial_number_buffer_size = sizeof(serial_number_buffer);
    CHECK(k4a_device_get_serialnum(device, serial_number_buffer, &serial_number_buffer_size), device);

    std::cout << "Device serial number: " << serial_number_buffer << std::endl;

    k4a_hardware_version_t version_info;
    CHECK(k4a_device_get_version(device, &version_info), device);

    std::cout << "Device version: " << (version_info.firmware_build == K4A_FIRMWARE_BUILD_RELEASE ? "Rel" : "Dbg")
              << "; C: " << version_info.rgb.major << "." << version_info.rgb.minor << "." << version_info.rgb.iteration
              << "; D: " << version_info.depth.major << "." << version_info.depth.minor << "."
              << version_info.depth.iteration << "[" << version_info.depth_sensor.major << "."
              << version_info.depth_sensor.minor << "]"
              << "; A: " << version_info.audio.major << "." << version_info.audio.minor << "."
              << version_info.audio.iteration << std::endl;

    uint32_t camera_fps = k4a_convert_fps_to_uint(device_config->camera_fps);

    if (camera_fps <= 0 || (device_config->color_resolution == K4A_COLOR_RESOLUTION_OFF &&
                            device_config->depth_mode == K4A_DEPTH_MODE_OFF))
    {
        std::cerr << "Either the color or depth modes must be enabled to record." << std::endl;
        return 1;
    }

    if (absoluteExposureValue != defaultExposureAuto)
    {
        if (K4A_FAILED(k4a_device_set_color_control(device,
                                                    K4A_COLOR_CONTROL_EXPOSURE_TIME_ABSOLUTE,
                                                    K4A_COLOR_CONTROL_MODE_MANUAL,
                                                    absoluteExposureValue)))
        {
            std::cerr << "Runtime error: k4a_device_set_color_control() for manual exposure failed " << std::endl;
        }
    }
    else
    {
        if (K4A_FAILED(k4a_device_set_color_control(device,
                                                    K4A_COLOR_CONTROL_EXPOSURE_TIME_ABSOLUTE,
                                                    K4A_COLOR_CONTROL_MODE_AUTO,
                                                    0)))
        {
            std::cerr << "Runtime error: k4a_device_set_color_control() for auto exposure failed " << std::endl;
        }
    }

    if (gain != defaultGainAuto)
    {
        if (K4A_FAILED(
                k4a_device_set_color_control(device, K4A_COLOR_CONTROL_GAIN, K4A_COLOR_CONTROL_MODE_MANUAL, gain)))
        {
            std::cerr << "Runtime error: k4a_device_set_color_control() for manual gain failed " << std::endl;
        }
    }

    CHECK(k4a_device_start_cameras(device, device_config), device);
    if (record_imu)
    {
        CHECK(k4a_device_start_imu(device), device);
    }

    std::cout << "Device started" << std::endl;

    k4a_record_t recording;
    if (K4A_FAILED(k4a_record_create(recording_filename, device, *device_config, &recording)))
    {
        std::cerr << "Unable to create recording file: " << recording_filename << std::endl;
        return 1;
    }

    if (record_imu)
    {
        CHECK(k4a_record_add_imu_track(recording), device);
    }
    CHECK(k4a_record_write_header(recording), device);

    // Wait for the first capture before starting recording.
    k4a_capture_t capture;
    seconds timeout_sec_for_first_capture(60);
    if (device_config->wired_sync_mode == K4A_WIRED_SYNC_MODE_SUBORDINATE)
    {
        timeout_sec_for_first_capture = seconds(360);
        std::cout << "[subordinate mode] Waiting for signal from master" << std::endl;
    }
    steady_clock::time_point first_capture_start = steady_clock::now();
    k4a_wait_result_t result = K4A_WAIT_RESULT_TIMEOUT;
    // Wait for the first capture in a loop so Ctrl-C will still exit.
    while (!exiting && (steady_clock::now() - first_capture_start) < timeout_sec_for_first_capture)
    {
        result = k4a_device_get_capture(device, &capture, 100);
        if (result == K4A_WAIT_RESULT_SUCCEEDED)
        {
            k4a_capture_release(capture);
            break;
        }
        else if (result == K4A_WAIT_RESULT_FAILED)
        {
            std::cerr << "Runtime error: k4a_device_get_capture() returned error: " << result << std::endl;
            return 1;
        }
    }

    if (exiting)
    {
        k4a_device_close(device);
        return 0;
    }
    else if (result == K4A_WAIT_RESULT_TIMEOUT)
    {
        std::cerr << "Timed out waiting for first capture." << std::endl;
        return 1;
    }

    std::cout << "Started recording" << std::endl;
    if (recording_length <= 0)
    {
        std::cout << "Press Ctrl-C to stop recording." << std::endl;
    }

    steady_clock::time_point recording_start = steady_clock::now();
    int32_t timeout_ms = 1000 / camera_fps;

    ////////////////////////////////////
    FILE *fpt;
    //fpt = fopen(argv[2], "w+");
    fpt = fopen(timestamps_table_filename, "w+");
    fprintf(fpt, "color_ts_us,depth_ts_us,global_ts_us\n");
    ////////////////////////////////////

    int frame_number = 0;
    do
    {
        result = k4a_device_get_capture(device, &capture, timeout_ms);
        if (result == K4A_WAIT_RESULT_TIMEOUT)
        {
            continue;
        }
        else if (result != K4A_WAIT_RESULT_SUCCEEDED)
        {
            std::cerr << "Runtime error: k4a_device_get_capture() returned " << result << std::endl;
            break;
        }
        CHECK(k4a_record_write_capture(recording, capture), device);

        ///////////////////////////////
        uint64_t global_timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        k4a_image_t color_image = k4a_capture_get_color_image(capture);
        k4a_image_t depth_image = k4a_capture_get_depth_image(capture);

        if (frame_number % 60 == 0 && color_image != NULL) {
            uint8_t* image_buffer = k4a_image_get_buffer(color_image);
            size_t image_buffer_size = k4a_image_get_size(color_image);

            char rgbname[50];
            sprintf(rgbname, "color_preview/%08d_color.jpg", frame_number);
            std::ofstream file_object(rgbname, std::ios::out | std::ios::binary);
            file_object.write(reinterpret_cast<char*>(image_buffer), image_buffer_size);
            file_object.close();
        }

        if (color_image && depth_image) {

            uint64_t color_image_timestamp = k4a_image_get_device_timestamp_usec(color_image);
            uint64_t depth_image_timestamp = k4a_image_get_device_timestamp_usec(depth_image);

            fprintf(fpt, "%ld,%ld,%ld\n", color_image_timestamp, depth_image_timestamp, global_timestamp);

            uint8_t * buffer = k4a_image_get_buffer(color_image);
            uint32_t size = k4a_image_get_size(color_image);

            FILE * pFile;
            char filename_buffer[50];

            sprintf (filename_buffer, "/mnt/mrob_tmpfs/%s_color.jpg", serial_number_buffer);
            pFile = fopen (filename_buffer, "w");

            if (pFile!=NULL)
            {
                fwrite(buffer, 1, size, pFile);
                fclose (pFile);
            }

            buffer = k4a_image_get_buffer(depth_image);
            size = k4a_image_get_size(depth_image);

            sprintf (filename_buffer, "/mnt/mrob_tmpfs/%s_depth.bin", serial_number_buffer);
            pFile = fopen (filename_buffer, "w");

            if (pFile!=NULL)
            {
                fwrite(buffer, 1, size, pFile);
                fclose (pFile);
            }
        }
        if (color_image) {
            k4a_image_release(color_image);
        }
        if (depth_image) {
            k4a_image_release(depth_image);
        }
        /////////////////////

        k4a_capture_release(capture);

        if (record_imu)
        {
            do
            {
                k4a_imu_sample_t sample;
                result = k4a_device_get_imu_sample(device, &sample, 0);
                if (result == K4A_WAIT_RESULT_TIMEOUT)
                {
                    break;
                }
                else if (result != K4A_WAIT_RESULT_SUCCEEDED)
                {
                    std::cerr << "Runtime error: k4a_imu_get_sample() returned " << result << std::endl;
                    break;
                }
                k4a_result_t write_result = k4a_record_write_imu_sample(recording, sample);
                if (K4A_FAILED(write_result))
                {
                    std::cerr << "Runtime error: k4a_record_write_imu_sample() returned " << write_result << std::endl;
                    break;
                }
            } while (!exiting && result != K4A_WAIT_RESULT_FAILED &&
                     (recording_length < 0 || (steady_clock::now() - recording_start < recording_length_seconds)));
        }
        frame_number++;
    } while (!exiting && result != K4A_WAIT_RESULT_FAILED &&
             (recording_length < 0 || (steady_clock::now() - recording_start < recording_length_seconds)));

    if (!exiting)
    {
        exiting = true;
        std::cout << "Stopping recording..." << std::endl;
    }

    if (record_imu)
    {
        k4a_device_stop_imu(device);
    }
    k4a_device_stop_cameras(device);

    std::cout << "Saving recording..." << std::endl;
    CHECK(k4a_record_flush(recording), device);
    k4a_record_close(recording);

    std::cout << "Done" << std::endl;

    k4a_device_close(device);

    return 0;
}
