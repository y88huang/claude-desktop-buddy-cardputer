"""
PlatformIO post-build hook that merges the three ESP32 flash regions
(bootloader + partition table + application) into a single .bin that can
be written at offset 0x0. This is the format M5Burner's "Open Custom
Firmware" accepts, and the same one-file-one-offset input esptool.py
write_flash wants.

Hooked in via:

    extra_scripts = post:scripts/merge_bin.py

in platformio.ini. Output lands in `release/<env>-merged.bin` so it's
easy to upload to a GitHub Release as an asset.
"""

Import("env")  # noqa: F821
import os


def merge_bin(source, target, env):
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    prog_name   = env.subst("$PROGNAME")
    pio_env     = env["PIOENV"]

    board_cfg   = env.BoardConfig()
    chip        = board_cfg.get("build.mcu", "esp32s3")

    # Use "keep" for flash mode / freq / size so esptool preserves
    # whatever the Arduino-ESP32 bootloader baked in. Passing concrete
    # values (read from BoardConfig) turned out to cause a header
    # mismatch on first boot — the bootloader couldn't read the app and
    # the device came up with a black screen. "keep" is safer and always
    # matches what `pio run -t upload` would flash.
    flash_mode  = "keep"
    flash_freq  = "keep"
    flash_size  = "keep"

    # ESP32-S3 / C3 boot at 0x0; classic ESP32 at 0x1000. PlatformIO's
    # board manifest carries the right offset per chip — fall back to 0x0
    # since our default target is the Cardputer's S3.
    boot_offset = board_cfg.get("upload.bootloader_offset", "0x0")

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware   = os.path.join(build_dir, f"{prog_name}.bin")

    release_dir = os.path.join(project_dir, "release")
    os.makedirs(release_dir, exist_ok=True)
    out_path = os.path.join(release_dir, f"{pio_env}-merged.bin")

    # PlatformIO's PYTHONEXE venv doesn't always have esptool installed as
    # a module — it's bundled as a standalone script in the tool-esptoolpy
    # package instead. Use the package path so this works regardless of
    # which Python PIO is running from.
    python_exe = env.subst("$PYTHONEXE")
    esptool_pkg = env.PioPlatform().get_package_dir("tool-esptoolpy")
    esptool_py = os.path.join(esptool_pkg, "esptool.py")
    cmd = (
        f'"{python_exe}" "{esptool_py}" '
        f"--chip {chip} merge_bin "
        f'-o "{out_path}" '
        f"--flash_mode {flash_mode} "
        f"--flash_freq {flash_freq} "
        f"--flash_size {flash_size} "
        f'{boot_offset} "{bootloader}" '
        f'0x8000 "{partitions}" '
        f'0x10000 "{firmware}"'
    )

    print(f"\nmerge_bin: producing {out_path}")
    rc = env.Execute(cmd)
    if rc != 0:
        print(f"merge_bin: esptool exited with status {rc}")
        return

    try:
        size = os.path.getsize(out_path)
        print(
            f"merge_bin: {size // 1024} KB ({size:,} bytes) — "
            f"flash at offset 0x0 on {chip}"
        )
    except OSError:
        print("merge_bin: WARNING — output file not found after merge")


# Fire once the main app .bin has been produced; bootloader.bin and
# partitions.bin are already written by earlier build steps.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)  # noqa: F821
