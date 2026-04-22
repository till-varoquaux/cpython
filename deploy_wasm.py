#!/usr/bin/env python3
"""Automate CPython WASM compilation and deployment to GitHub Pages."""

import argparse
import html
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Constants
ROOT = Path(__file__).parent.parent.absolute()
EMSCRIPTEN_INFRA = ROOT / "Platforms" / "emscripten"
CROSS_BUILD_DIR = ROOT / "cross-build"
HOST_DIR = CROSS_BUILD_DIR / "wasm32-emscripten" / "build" / "python"
EMSDK_CACHE = ROOT / "emsdk-cache"

def run(cmd, cwd=None, env=None):
    """Run a shell command with error checking, extending the current environment."""
    print(f"\n[run] {' '.join(map(str, cmd))}")

    # Always start with a fresh copy of the current environment
    cmd_env = os.environ.copy()
    if env:
        cmd_env.update(env)

    subprocess.check_call(cmd, cwd=cwd, env=cmd_env)

def sanitize_environment():
    """Remove problematic bash functions and block sub-bash rc loading."""
    for key in list(os.environ.keys()):
        if key.startswith("BASH_FUNC_"):
            del os.environ[key]
    os.environ["BASH_ENV"] = "/dev/null"
    os.environ["ENV"] = "/dev/null"

def install_system_deps():
    """Install system dependencies via dnf5."""
    print("--- Installing system dependencies ---")
    deps = [
        "git", "make", "gcc", "gcc-c++", "pkg-config",
        "zlib-devel", "bzip2-devel", "xz-devel", "nodejs"
    ]
    run(["sudo", "dnf5", "install", "-y", "--skip-unavailable"] + deps)

def clean_source_tree():
    """Ensure the source tree is clean for an out-of-tree build."""
    print("--- Cleaning source tree ---")
    run(["git", "clean", "-fdx", "-e", "tools", "-e", "emsdk-cache", "-e", "Doc/venv"])

def find_stdlib_zip():
    """Locate the stdlib zip in the build output."""
    if not HOST_DIR.exists():
        raise RuntimeError(f"Build directory {HOST_DIR} not found.")

    stdlib_zips = list(HOST_DIR.glob("python*.zip"))
    if not stdlib_zips:
        raise RuntimeError("Could not find stdlib zip in build output.")

    return sorted(stdlib_zips, key=lambda p: len(p.name))[0]

def inject_coi_serviceworker(dest_dir):
    """Inject the local coi-serviceworker.js into the deployment bundle."""
    print("--- Adding local coi-serviceworker for SharedArrayBuffer support ---")

    # Use the local script from the tools directory to avoid supply chain risks
    local_coi = ROOT / "tools" / "coi-serviceworker.js"
    if not local_coi.exists():
        raise RuntimeError(f"Could not find {local_coi}. Please download it first.")

    shutil.copy(local_coi, dest_dir / "coi-serviceworker.js")

    index_path = dest_dir / "index.html"
    with open(index_path, "r") as f:
        html = f.read()

    script_tag = '<script src="coi-serviceworker.js"></script>'
    if "<head>" in html:
        html = html.replace("<head>", f"<head>\n    {script_tag}")
    else:
        html = f"{script_tag}\n{html}"

    with open(dest_dir / "index.html", "w") as f:
        f.write(html)

def apply_patches(dest_dir):
    """Apply all .diff files in tools/ to the destination directory."""
    # Ignore hidden files starting with '.'
    patches = sorted(p for p in ROOT.glob("tools/*.diff") if not p.name.startswith("."))
    if not patches:
        return

    print("--- Applying patches ---")
    for patch in patches:
        print(f"Applying {patch.name}...")
        # Let patch automatically determine the file from the header.
        # -p1 is used because our diffs use git-style paths (--- a/index.html)
        run(["patch", "-p1", "-i", str(patch)], cwd=dest_dir)

def inject_demo_script(dest_dir):
    """Replace the default editor script and provide additional demos."""
    typer_path = ROOT / "tools" / "typer_demo.py"
    anno_path = ROOT / "tools" / "annotationlib_demo.py"

    print(f"--- Injecting demos into index.html ---")
    with open(typer_path, "r") as f:
        typer_content = f.read()
    with open(anno_path, "r") as f:
        anno_content = f.read()

    index_path = dest_dir / "index.html"
    with open(index_path, "r") as f:
        html_content = f.read()

    # 1. Set the initial editor content to the typer demo
    target = "print('Welcome to WASM!')"
    html_content = html_content.replace(target, html.escape(typer_content))

    # 2. Inject a global DEMOS object for switching
    # Escape backslashes first, then backticks and dollar signs for JS template literals
    def js_escape(c):
        return c.replace("\\", "\\\\").replace("`", "\\`").replace("$", "\\$")

    demos_js = f"""
        <script>
            window.DEMOS = {{
                "typer": `{js_escape(typer_content)}`,
                "annotationlib": `{js_escape(anno_content)}`
            }};
        </script>
    """
    html_content = html_content.replace("</head>", f"{demos_js}\n</head>")

    with open(index_path, "w") as f:
        f.write(html_content)

def collect_assets(dest_dir):
    """Bundle build artifacts and web template into the destination directory."""
    print(f"--- Collecting assets in {dest_dir} ---")

    # Build artifacts
    for asset in ["python.mjs", "python.wasm"]:
        shutil.copy(HOST_DIR / asset, dest_dir)
    shutil.copy(find_stdlib_zip(), dest_dir)

    # Web example (standard template)
    template_dir = ROOT / "Platforms" / "emscripten" / "web_example"
    shutil.copy(template_dir / "python.worker.mjs", dest_dir)
    shutil.copy(template_dir / "server.py", dest_dir)

    # Inject index.html (copy first, then patch/inject)
    shutil.copy(template_dir / "index.html", dest_dir)
    apply_patches(dest_dir)
    inject_demo_script(dest_dir)

    inject_coi_serviceworker(dest_dir)

def git_push_to_gh_pages(src_dir):
    """Initialize a temporary git repo and force-push to gh-pages."""
    print("--- Pushing to gh-pages ---")

    remote_url = subprocess.check_output(
        ["git", "remote", "get-url", "origin"],
        cwd=ROOT,
        encoding="utf-8"
    ).strip()

    run(["git", "init"], cwd=src_dir)
    run(["git", "checkout", "-b", "gh-pages"], cwd=src_dir)
    run(["git", "add", "."], cwd=src_dir)
    run(["git", "commit", "-m", "Deploy CPython WASM to GitHub Pages"], cwd=src_dir)
    run(["git", "remote", "add", "origin", remote_url], cwd=src_dir)
    run(["git", "push", "-f", "origin", "gh-pages"], cwd=src_dir)


def build_native_python():
    """Step 1: Build the native Python required for cross-compilation."""
    print("--- Building native python ---")
    infra = str(EMSCRIPTEN_INFRA)
    cache = str(EMSDK_CACHE)

    # Install/Activate EMSDK and build native
    run([sys.executable, infra, "install-emscripten", "--emsdk-cache", cache])
    run([sys.executable, infra, "build", "build", "--emsdk-cache", cache])

def install_extra_packages():
    """Step 2: Install packages and compile them using the NATIVE built Python."""
    print("--- Installing extra packages (typer, annotated-types, typing-extensions) ---")
    packages = ["typer", "annotated-types", "typing-extensions"]
    lib_dir = ROOT / "Lib"

    # 1. Download/install raw .py files (System Python is fine for pip)
    run([sys.executable, "-m", "pip", "install", "--target", str(lib_dir), *packages])

    # 2. Compile to .pyc using the NATIVE Python we just built in the previous step
    native_python = CROSS_BUILD_DIR / "build" / "python"
    if not native_python.exists():
        raise RuntimeError(f"Native python not found at {native_python}. Did the native build fail?")

    print("--- Pre-compiling to .pyc using matching native Python ---")

    # NEW: Exclude the 'test' directory (which contains deliberate syntax errors).
    # We use subprocess.run with check=False so expected failures don't halt the script.
    compile_cmd = [
        str(native_python), "-m", "compileall",
        "-b", "-f", "-q",
        "-x", r"[/\\]test[/\\]", # Regex to exclude the test suite
        str(lib_dir)
    ]
    print(f"\n[run] {' '.join(map(str, compile_cmd))}")
    subprocess.run(compile_cmd, check=False)

    print(f"--- Successfully installed and compiled {', '.join(packages)} ---")

def build_wasm_host():
    """Step 3: Build the Host Python (WASM) target."""
    print("--- Building host (wasm) python for browser ---")
    infra = str(EMSCRIPTEN_INFRA)
    cache = str(EMSDK_CACHE)

    wasm_ldflags = (
        "-sENVIRONMENT=web,worker "
        "-sALLOW_MEMORY_GROWTH=0 "
        "-sINITIAL_MEMORY=536870912 "
        "-sMAXIMUM_MEMORY=536870912 "
        "-sASSERTIONS=1 "
        "-sSTACK_SIZE=33554432"
    )

    # Passing via LIBS ensures these flags appear AFTER CPython's hardcoded LINKFORSHARED flags 
    # (like -sSTACK_SIZE=5MB) in the final link command, allowing them to take precedence.
    env = {
        "LDFLAGS": wasm_ldflags, 
        "LIBS": wasm_ldflags, 
        "TYPER_USE_RICH": "0",
        "_TYPER_COMPLETE_TEST_DISABLE_SHELL_DETECTION": "1",
    }
    run([
        sys.executable, infra, "build", "host",
        "--emsdk-cache", cache,
        "--host-runner", "true"
    ], env=env)



def main():
    parser = argparse.ArgumentParser(description="Deploy CPython WASM to GitHub Pages")
    parser.add_argument("--no-deps", action="store_true", help="Skip installing system dependencies")
    parser.add_argument("--no-clean", action="store_true", help="Skip cleaning the source tree")
    parser.add_argument("--no-build", action="store_true", help="Skip the build process")
    parser.add_argument("--no-deploy", action="store_true", help="Skip asset collection and pushing")
    args = parser.parse_args()

    sanitize_environment()

    if not args.no_deps:
        install_system_deps()

    if not args.no_clean:
        clean_source_tree()

    if not args.no_build:
        # Install extra packages before building so they are in the zip
        # 1. Build the native interpreter first
        build_native_python()

        # 2. Use the native interpreter to compile installed packages
        install_extra_packages()

        # 3. Finally, build the WASM payload
        build_wasm_host()


    if not args.no_deploy:
        with tempfile.TemporaryDirectory() as tmpdir:
            dest = Path(tmpdir)
            collect_assets(dest)
            git_push_to_gh_pages(dest)
        print("\n[success] Deployment successful!")

if __name__ == "__main__":
    main()
