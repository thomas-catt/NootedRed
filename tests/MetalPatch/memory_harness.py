"""将公共页面工具注入已有 Mach API 模拟环境，执行生产实现。"""
from pathlib import Path
import re


def with_memory_helpers(body):
    if "MetalPatch::Memory::" not in body:
        return body
    header = Path(__file__).resolve().parents[2] / "NootedRed/MetalPatch/Memory.hpp"
    helpers = re.sub(r"^#(?:include|pragma).*\n", "", header.read_text(), flags=re.MULTILINE)
    return helpers + "\n" + body
