# Copyright (c) Meta Platforms, Inc. and affiliates.

from setuptools import find_packages, setup

_packages = find_packages()

setup(
    name="ai4animation",
    version="1.0.0",
    description="AI4Animation Python Framework - Neural network-based character animation",
    author="Paul Starke, Sebastian Starke",
    packages=_packages,
    package_dir={"ai4animation": "ai4animation"},
    entry_points={
        "console_scripts": [
            "convert=ai4animation.Import.BatchConverter:main",
        ],
    },
    python_requires=">=3.12.12",
    install_requires=[
        "raylib==5.5",
        "numpy==1.26.4",
        "pygltflib==1.16.5",
        "pyscreenrec==0.6",
        "torch>=2.0.0",
        "torchvision>=0.15.0",
        "torchaudio>=2.0.0",
        "scipy>=1.7.0",
        "matplotlib>=3.10.3",
        "scikit-learn>=1.7.1",
        "einops>=0.8.1",
        "tqdm",
        "pyyaml",
        "sounddevice>=0.5.0",
        "soundfile>=0.12.0",
    ],
)
