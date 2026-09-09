"""Use installed LLVM on Windows when GCC is unavailable; keep GCC elsewhere."""

import os
import shutil
from pathlib import Path

Import("env")

if os.name == "nt" and not (shutil.which("gcc") and shutil.which("g++")):
    llvm = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin"
    if (llvm / "clang.exe").is_file() and (llvm / "clang++.exe").is_file():
        toolchain = dict(
            CC='"' + (llvm / "clang.exe").as_posix() + '"',
            CXX='"' + (llvm / "clang++.exe").as_posix() + '"',
            LINK='"' + (llvm / "clang++.exe").as_posix() + '"',
            AR='"' + (llvm / "llvm-ar.exe").as_posix() + '"',
            RANLIB='"' + (llvm / "llvm-ranlib.exe").as_posix() + '"',
        )

        def use_llvm(build_env, node):
            build_env.Replace(**toolchain)
            env.Replace(**toolchain)
            return node

        env.AddBuildMiddleware(use_llvm)
