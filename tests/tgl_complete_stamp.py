"""Compile and run the completeStamp cases against the real recovery header."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='xe-complete-stamp-') as directory:
    binary = Path(directory)/'complete_stamp'
    subprocess.run(['clang++','-std=c++17','-Wall','-Werror','-O1',str(root/'tests/tgl_complete_stamp.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
