"""Packs the Android build into an APK without Gradle.

javac (SDL3's Java sources and android/java) -> d8 -> aapt2 link with
android/AndroidManifest.xml -> stripped .so files under lib/<abi>/ ->
zipalign -> apksigner with a local debug key.

Usage: python tools/android/build_apk.py [--build out/build/android-x86_64]
           [--abi x86_64] [--out out/dist/broken-bond-x86_64.apk]
The SDK and NDK come from ANDROID_HOME (default D:/LaHaine/Tools/AndroidTools).
"""

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get("ANDROID_HOME", "D:/LaHaine/Tools/AndroidTools"))
NDK = SDK / "ndk" / "30.0.16248370"
BUILD_TOOLS = SDK / "build-tools" / "36.1.0"
PLATFORM = SDK / "platforms" / "android-36" / "android.jar"
JAVA_HOME = Path(os.environ.get("JAVA_HOME", "C:/Program Files/Eclipse Adoptium/jdk-25.0.1.8-hotspot"))
SDL_JAVA = ROOT / "thirdparty/rexglue-sdk/thirdparty/sdl3/android-project/app/src/main/java"
EXE = ".exe" if os.name == "nt" else ""
BAT = ".bat" if os.name == "nt" else ""


def run(*args):
    print("+", " ".join(str(a) for a in args[:4]), "..." if len(args) > 4 else "")
    subprocess.run([str(a) for a in args], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="out/build/android-x86_64")
    parser.add_argument("--abi", default="x86_64")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    build = (ROOT / args.build).resolve()
    out = Path(args.out) if args.out else ROOT / "out/dist" / f"broken-bond-{args.abi}.apk"
    out = out if out.is_absolute() else ROOT / out
    work = build / "apk"
    shutil.rmtree(work, ignore_errors=True)
    (work / "classes").mkdir(parents=True)

    # Java.
    sources = sorted(SDL_JAVA.rglob("*.java")) + sorted((ROOT / "android/java").rglob("*.java"))
    (work / "sources.txt").write_text("\n".join(str(s) for s in sources), encoding="utf-8")
    run(JAVA_HOME / "bin" / f"javac{EXE}", "-nowarn", "-Xlint:-options", "-encoding", "UTF-8",
        "-source", "8", "-target", "8",
        "-bootclasspath", os.pathsep.join([str(PLATFORM), str(BUILD_TOOLS / "core-lambda-stubs.jar")]),
        "-d", work / "classes", f"@{work / 'sources.txt'}")
    classes = sorted((work / "classes").rglob("*.class"))
    run(BUILD_TOOLS / f"d8{BAT}", "--release", "--min-api", "30", "--lib", PLATFORM,
        "--output", work, *classes)

    # Manifest (no resources of our own).
    unaligned = work / "unaligned.apk"
    run(BUILD_TOOLS / f"aapt2{EXE}", "link", "-o", unaligned, "-I", PLATFORM,
        "--manifest", ROOT / "android/AndroidManifest.xml")

    # Native libraries, stripped. The unstripped ones stay in the build tree
    # for symbolizing tombstones.
    libs = [build / "libmain.so", build / "librexgpu-hostrd.so"]
    sdk_out = ROOT / "thirdparty/rexglue-sdk/out" / f"android-{args.abi}"
    libs += [sdk_out / "librexruntimerd.so", sdk_out / "libTracyClientrd.so"]
    strip = NDK / "toolchains/llvm/prebuilt/windows-x86_64/bin" / f"llvm-strip{EXE}"
    stripped = work / "lib"
    stripped.mkdir()
    for lib in libs:
        if not lib.is_file():
            sys.exit(f"missing {lib}")
        run(strip, "--strip-unneeded", "-o", stripped / lib.name, lib)

    with zipfile.ZipFile(unaligned, "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(work / "classes.dex", "classes.dex")
        for lib in sorted(stripped.iterdir()):
            apk.write(lib, f"lib/{args.abi}/{lib.name}")

    aligned = work / "aligned.apk"
    run(BUILD_TOOLS / f"zipalign{EXE}", "-f", "-p", "4", unaligned, aligned)

    # A throwaway debug key, made once per checkout (not a secret).
    keystore = ROOT / "out/build/android-debug.keystore"
    if not keystore.exists():
        run(JAVA_HOME / "bin" / f"keytool{EXE}", "-genkeypair", "-keystore", keystore,
            "-storepass", "android", "-keypass", "android", "-alias", "debug",
            "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
            "-dname", "CN=Broken Bond Debug")
    out.parent.mkdir(parents=True, exist_ok=True)
    run(BUILD_TOOLS / f"apksigner{BAT}", "sign", "--ks", keystore, "--ks-pass", "pass:android",
        "--key-pass", "pass:android", "--out", out, aligned)
    print(f"{out} ({out.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
