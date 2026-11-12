import os
import subprocess

Import("env")


def merge_firmware(target, source, env):
    board = env.BoardConfig()
    esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    output = env.subst(os.path.join("$BUILD_DIR", "firmware-merged.bin"))
    command = [
        env.subst("$PYTHONEXE"),
        esptool,
        "--chip",
        env.subst("$BOARD_MCU"),
        "merge_bin",
        "--output",
        output,
        "--flash_mode",
        env["__get_board_flash_mode"](env),
        "--flash_freq",
        env["__get_board_f_flash"](env),
        "--flash_size",
        board.get("upload.flash_size", "4MB"),
    ]
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
        command.extend([env.subst(offset), env.subst(image)])
    command.extend([env.subst("$ESP32_APP_OFFSET"), env.subst(os.path.join("$BUILD_DIR", "${PROGNAME}.bin"))])
    subprocess.check_call(command)
    print("Merged firmware: {}".format(output))


images = [env.subst(image) for _, image in env.get("FLASH_EXTRA_IMAGES", [])]
images.append(env.subst(os.path.join("$BUILD_DIR", "${PROGNAME}.bin")))
env.AddCustomTarget(
    name="merge",
    dependencies=images,
    actions=merge_firmware,
    title="Merge firmware",
    description="Build a single flashable ESP32 image",
)
