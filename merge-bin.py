#!/usr/bin/python3

# Adds PlatformIO post-processing to merge all the ESP flash images into a single image.

import os
import subprocess

Import("env", "projenv")

board_config = env.BoardConfig()
firmware_bin = "${BUILD_DIR}/${PROGNAME}.bin"
merged_bin = os.environ.get("MERGED_BIN_PATH", "${BUILD_DIR}/${PROGNAME}-merged.bin")


def merge_bin_action(source, target, env):
    flash_images = [
        *env.Flatten(env.get("FLASH_EXTRA_IMAGES", [])),
        "$ESP32_APP_OFFSET",
        source[0].get_abspath(),
    ]
    # Pass separate arguments so project/output paths may contain spaces.
    merge_cmd = [
        "$PYTHONEXE",
        "$OBJCOPY",
        "--chip",
        board_config.get("build.mcu", "esp32"),
        "merge_bin",
        "-o",
        merged_bin,
        "--flash_mode",
        board_config.get("build.flash_mode", "dio"),
        "--flash_freq",
        "${__get_board_f_flash(__env__)}",
        "--flash_size",
        board_config.get("upload.flash_size", "4MB"),
        *flash_images,
    ]
    # Propagate failures to PlatformIO instead of reporting a successful merge.
    return subprocess.call([env.subst(str(arg)) for arg in merge_cmd],
                           cwd=env.subst("$PROJECT_DIR"))


env.AddCustomTarget(
    name="mergebin",
    dependencies=firmware_bin,
    actions=merge_bin_action,
    title="Merge binary",
    description="Build combined image",
    always_build=True,
)
