# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The oot2-native contributors
"""The GPU BC7 encoder (runtime/gpu/shaders/bc7_encode.comp; the ASTC transcode), through tests/runtime/bc7_encode_test.c.

The program makes its own Vulkan 1.3 instance and device and links no runtime code. It encodes seeded images (a gradient, a ramp
with noise, a colour edge, a smooth blob, an alpha ramp, white noise; sizes that are not multiples of four among them), decodes
every block with the format's mode 6 as the specification reads it, and holds each kind of image to a floor of PSNR. Every block
must name mode 6 (the encoder writes no other).

- the shader as built keeps every kind of image above its floor;
- counter-run: a copy of the shader that writes the endpoints in the wrong order (the second before the first) is caught.

The block layout was also checked once against an independent decoder (Pillow's) on a frame of the game: 46 dB.

Without clang or the Vulkan SDK the tests skip, unless OOT2_REQUIRE_CLANG or OOT2_REQUIRE_VULKAN is set; without a usable device
they skip, unless OOT2_REQUIRE_GPU is set.

Run: python -m unittest discover -s tests -p test_runtime_vk_bc7.py -v
"""

from __future__ import annotations

import atexit
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_runtime_gpu import EXE, ROOT, case_env, clang_or_skip, compile_now, vulkan_or_skip

SHADER = ROOT / "runtime" / "gpu" / "shaders" / "bc7_encode.comp"
SOURCE = ROOT / "tests" / "runtime" / "bc7_encode_test.c"
EXIT_NO_DEVICE = 3

# the endpoints swapped in the writing: every block then decodes with its two colours the wrong way round
SWAP_FROM = "put(b, pos, q0[c], 7u), pos += 7u;\n        put(b, pos, q1[c], 7u), pos += 7u;"
SWAP_TO = "put(b, pos, q1[c], 7u), pos += 7u;\n        put(b, pos, q0[c], 7u), pos += 7u;"

_state: dict[str, object] = {}


def tools() -> tuple[Path, str, Path]:
    """The harness program, glslang, and a folder for compiled shaders; built once."""
    if "exe" not in _state:
        cc = clang_or_skip()
        include, glslang = vulkan_or_skip()
        tmp = tempfile.TemporaryDirectory(prefix="oot2-bc7-", ignore_cleanup_errors=True)
        atexit.register(tmp.cleanup)
        _state["tmp"] = tmp
        exe = Path(tmp.name) / f"bc7_encode_test{EXE}"
        cmd = [cc, "-std=gnu11", "-O2", "-march=x86-64-v3", "-Wall", "-Wextra", "-Werror", "-I", str(include),
               "-I", str(include / "Volk"), str(SOURCE), "-lm", "-o", str(exe)]
        done = subprocess.run(cmd, capture_output=True, text=True)
        if done.returncode != 0:
            raise RuntimeError(f"build of bc7_encode_test failed:\n{done.stdout}{done.stderr}")
        _state["exe"] = exe
        _state["glslang"] = glslang
    return _state["exe"], _state["glslang"], Path(_state["tmp"].name)  # type: ignore[union-attr]


def compile_shader(source: str, name: str) -> Path:
    exe, glslang, tmp = tools()
    comp = tmp / f"{name}.comp"
    comp.write_text(source, encoding="utf-8")
    spv = tmp / f"{name}.spv"
    done = subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", str(comp), "-o", str(spv)], capture_output=True,
                          text=True)
    if done.returncode != 0:
        raise RuntimeError(f"glslang failed on {name}:\n{done.stdout}{done.stderr}")
    return spv


def run(spv: Path) -> subprocess.CompletedProcess:
    exe, _, _ = tools()
    return subprocess.run([str(exe), str(spv)], capture_output=True, text=True, timeout=300, env=case_env())


class Bc7EncodeTest(unittest.TestCase):
    def run_or_skip(self, spv: Path) -> subprocess.CompletedProcess:
        done = run(spv)
        if done.returncode == EXIT_NO_DEVICE and not os.environ.get("OOT2_REQUIRE_GPU"):
            self.skipTest(done.stdout.strip() or "no usable Vulkan device")
        return done

    def test_every_kind_of_image_keeps_its_floor(self) -> None:
        source = SHADER.read_text(encoding="utf-8")
        done = self.run_or_skip(compile_shader(source, "bc7_encode"))
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("ok bc7_encode", done.stdout)

    def test_counter_run_endpoints_written_the_wrong_way_round_are_caught(self) -> None:
        source = SHADER.read_text(encoding="utf-8")
        self.assertIn(SWAP_FROM, source)
        done = self.run_or_skip(compile_shader(source.replace(SWAP_FROM, SWAP_TO), "bc7_encode_swapped"))
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertNotIn("ok bc7_encode", done.stdout)


if __name__ == "__main__":
    unittest.main()
