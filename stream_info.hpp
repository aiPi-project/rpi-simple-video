/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2021, Raspberry Pi (Trading) Ltd.
 *
 * stream_info.hpp - structure holding details about a libcamera Stream.
 */
#pragma once

#include <libcamera/color_space.h>
#include <libcamera/pixel_format.h>

#include <optional>

struct StreamInfo {
    StreamInfo() : width(0), height(0), stride(0) {}
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    libcamera::PixelFormat pixel_format;
    std::optional<libcamera::ColorSpace> colour_space;
};
