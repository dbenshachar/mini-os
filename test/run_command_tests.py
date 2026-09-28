#!/usr/bin/env python3
import glob
import os
import subprocess
import sys


def parse_case(path):
    sections = {"COMMANDS": [], "EXPECT": []}
    current = None
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if stripped in sections:
                current = stripped
                continue
            if current is None:
                raise AssertionError(f"{path}: content before a section header: {line}")
            sections[current].append(line)
    if not sections["COMMANDS"]:
        raise AssertionError(f"{path}: missing COMMANDS")
    if not sections["EXPECT"]:
        raise AssertionError(f"{path}: missing EXPECT")
    return sections


def run_os(root, disk, commands, timeout=20):
    qemu = os.environ.get("QEMU", "qemu-system-aarch64")
    kernel = os.path.join(root, "kernel.elf")
    proc = subprocess.Popen(
        [
            qemu,
            "-M", "virt",
            "-cpu", "cortex-a57",
            "-nographic",
            "-serial", "mon:stdio",
            "-kernel", kernel,
            "-drive", f"if=none,file={disk},format=raw,id=hd0",
            "-device", "virtio-blk-device,drive=hd0",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        out, _ = proc.communicate(commands, timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        raise AssertionError("QEMU timed out. Partial output:\n" + out)
    if proc.returncode != 0:
        raise AssertionError(f"QEMU exited with {proc.returncode}. Output:\n{out}")
    return out


def require_output(out, expectations, output_path):
    for expected in expectations:
        expected = expected.replace("\\x1b", "\x1b").replace("\\n", "\n")
        if expected not in out:
            raise AssertionError(
                f"missing expected output: {expected!r}\nFull output saved to {output_path}"
            )


def case_name(path):
    return os.path.splitext(os.path.basename(path))[0]


def main():
    if len(sys.argv) < 3:
        print("usage: run_command_tests.py ROOT test/commands/*.test", file=sys.stderr)
        return 2

    root = sys.argv[1]
    patterns = sys.argv[2:]
    tests = []
    for pattern in patterns:
        tests.extend(glob.glob(pattern))
    tests = sorted(set(tests))
    if not tests:
        raise AssertionError("no command test files found")

    output_dir = os.path.join(root, "out", "test-outputs")
    os.makedirs(output_dir, exist_ok=True)

    for test_path in tests:
        parsed = parse_case(test_path)
        name = case_name(test_path)
        disk = os.path.join(root, "out", f"{name}.img")
        output_path = os.path.join(output_dir, f"{name}.out")

        subprocess.check_call([os.path.join(root, "out", "mkfat32"), disk])
        commands = "\n".join(parsed["COMMANDS"]) + "\n"
        out = run_os(root, disk, commands)
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(out)
        require_output(out, parsed["EXPECT"], output_path)
        print(f"PASS {name}")

    print("mini_os command tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
