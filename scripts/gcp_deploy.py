#!/usr/bin/env python3
"""
GCP deploy script for MiniOSv UEFI kernel.

Flow:
    loader.img → raw tar.gz → GCS upload → custom image → VM
    (serial console output only, no external IP)

Usage:
    python3 gcp_deploy.py --login                        # fresh gcloud login (~1hr token)
    python3 gcp_deploy.py <image> <project> <arch>       # deploy
    python3 gcp_deploy.py --login <image> <project> <arch>

    <arch>  x86  → c4-standard-4   (Intel 4th-gen, x86_64)
            arm  → c4a-standard-4  (Google Axion, ARM64)

Prerequisites:
    pip install google-auth google-cloud-storage google-cloud-compute
    gcloud CLI must be installed (https://cloud.google.com/sdk/install)

Examples:
    python3 gcp_deploy.py --login loader.img my-project x86
    python3 gcp_deploy.py --login loader.img my-project arm
"""

import sys
import os
import time
import subprocess
import argparse
import tempfile

from google.oauth2.credentials import Credentials
from google.cloud import storage
import google.cloud.compute_v1 as compute_v1

# ── Config ────────────────────────────────────────────────────────────────────

REGION        = "europe-west1"      # Belgium — same region as your Azure setup
ZONE          = "europe-west1-b"
BUCKET_SUFFIX = "miniosv-images"    # bucket = {project}-{BUCKET_SUFFIX}
DISK_SIZE_GB  = 10

# Per-arch config: machine type, GCP architecture string, required image features
ARCH_CONFIG = {
    "x86": {
        "machine_type":      "c4-standard-4",
        "gcp_arch":          "X86_64",
        "guest_os_features": ["UEFI_COMPATIBLE", "GVNIC", "VIRTIO_SCSI_MULTIQUEUE"],
        "disk_type":         "hyperdisk-balanced",   # required for C4
    },
    "arm": {
        "machine_type":      "c4a-standard-4",
        "gcp_arch":          "ARM64",
        "guest_os_features": ["UEFI_COMPATIBLE", "GVNIC"],
        "disk_type":         "hyperdisk-balanced",   # required for C4A
    },
}

# ── Auth — gcloud-based, no persistent project credentials ────────────────────

def get_credentials(force_login: bool) -> Credentials:
    """
    Uses gcloud for auth — the same trusted flow as the gcloud CLI itself.

    --login  → runs `gcloud auth login --no-launch-browser` which prints a URL.
               Open it, authenticate, paste the code back into the terminal.
               Token is stored only in gcloud's own cache (~1 hour).
               Run `gcloud auth revoke` afterward to fully clear it.
    No flag  → uses `gcloud auth print-access-token` to get the current token.
               Fails with a clear message if not logged in.
    """
    if force_login:
        print("[auth] Starting gcloud login…")
        subprocess.run(
            ["gcloud", "auth", "login", "--no-launch-browser", "--brief"],
            check=True,
        )
        print("[auth] Login successful.")

    # Pull a short-lived access token from gcloud's cache
    try:
        token = subprocess.check_output(
            ["gcloud", "auth", "print-access-token"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except subprocess.CalledProcessError:
        print("[auth] Error: not logged in. Run with --login first.")
        sys.exit(1)

    print("[auth] Got access token from gcloud (valid ~1 hour).")
    # Wrap as a Credentials object — token only, no refresh needed within the hour
    return Credentials(token=token)

# ── 1. Prepare ────────────────────────────────────────────────────────────────

def prepare_tar(src: str, work_dir: str) -> str:
    """
    GCP image import requires a tar.gz containing a file named exactly 'disk.raw'.
    loader.img is already raw so we just rename it inside the archive.
    """
    tar_path = os.path.join(work_dir, "disk.tar.gz")
    print(f"[prepare] Packing {src} → disk.raw.tar.gz…")
    subprocess.run(
        [
            "tar", "-czf", tar_path,
            "-C", os.path.dirname(os.path.abspath(src)),
            "--transform", f"s/{os.path.basename(src)}/disk.raw/",
            os.path.basename(src),
        ],
        check=True,
    )
    print(f"[prepare] Done ({os.path.getsize(tar_path) / (1024**2):.1f} MiB)")
    return tar_path

# ── 2. Upload ─────────────────────────────────────────────────────────────────

def ensure_bucket(storage_client: storage.Client, project: str) -> str:
    """Create the GCS bucket in REGION if it doesn't exist. Returns bucket name."""
    bucket_name = f"{project}-{BUCKET_SUFFIX}"
    try:
        storage_client.get_bucket(bucket_name)
        print(f"[gcs] Using existing bucket gs://{bucket_name}")
    except Exception:
        print(f"[gcs] Creating bucket gs://{bucket_name} in {REGION}…")
        storage_client.create_bucket(bucket_name, location=REGION)
    return bucket_name


def upload_to_gcs(storage_client: storage.Client, bucket_name: str,
                  local_path: str, blob_name: str) -> str:
    """Upload file to GCS with progress. Returns gs:// URI."""
    file_size = os.path.getsize(local_path)
    print(f"[upload] {local_path} → gs://{bucket_name}/{blob_name}")
    blob = storage_client.bucket(bucket_name).blob(blob_name)
    blob.upload_from_filename(local_path)
    print(f"[upload] Done ({file_size / (1024**2):.1f} MiB)")
    return f"gs://{bucket_name}/{blob_name}"

# ── 3. Custom image ───────────────────────────────────────────────────────────

def create_image(project: str, image_name: str, gcs_uri: str, arch: str, credentials) -> str:
    """
    Import the GCS tar.gz as a GCP custom image with UEFI_COMPATIBLE.
    Uses gcloud CLI so auth flows through the same token as the rest of the script.
    Returns the image self-link.
    """
    cfg = ARCH_CONFIG[arch]
    features = ",".join(cfg["guest_os_features"])
    print(f"[image] Creating image '{image_name}' ({cfg['gcp_arch']})…")

    subprocess.run(
        [
            "gcloud", "compute", "images", "create", image_name,
            f"--source-uri={gcs_uri}",
            f"--guest-os-features={features}",
            f"--architecture={cfg['gcp_arch']}",
            f"--project={project}",
            "--quiet",
        ],
        check=True,
    )
    link = f"projects/{project}/global/images/{image_name}"
    print(f"[image] Ready: {link}")
    return link

# ── 4. VM ─────────────────────────────────────────────────────────────────────

def launch_vm(project: str, vm_name: str, image_self_link: str, arch: str, credentials):
    """
    Launch a VM with no external IP — serial console only.
    Uses gcloud CLI so auth flows through the same token as the rest of the script.
    """
    cfg = ARCH_CONFIG[arch]
    print(f"[vm] Launching {cfg['machine_type']} VM '{vm_name}' in {ZONE}…")

    subprocess.run(
        [
            "gcloud", "compute", "instances", "create", vm_name,
            f"--machine-type={cfg['machine_type']}",
            f"--image={image_self_link}",
            f"--boot-disk-size={DISK_SIZE_GB}GB",
            f"--boot-disk-type={cfg['disk_type']}",
            f"--zone={ZONE}",
            f"--project={project}",
            "--no-address",                          # no external IP
            "--metadata=serial-port-enable=1",
            "--labels=miniosv=true",
            "--quiet",
        ],
        check=True,
    )
    print(f"[vm] Running: {vm_name}")

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Deploy MiniOSv UEFI kernel to GCP")
    parser.add_argument("image",   nargs="?", default="loader.img",
                        help="Disk image to deploy (default: loader.img)")
    parser.add_argument("project", nargs="?", default=None,
                        help="GCP project ID")
    parser.add_argument("arch",    nargs="?", default="x86",
                        choices=["x86", "arm"],
                        help="x86 = c4-standard-4 | arm = c4a-standard-4")
    parser.add_argument("--login", action="store_true",
                        help="Force a fresh gcloud login before deploying")
    args = parser.parse_args()

    if not args.project:
        print("Error: GCP project ID is required.")
        print(f"Usage: {sys.argv[0]} [--login] <image> <project> <arch>")
        sys.exit(1)

    if not os.path.exists(args.image):
        print(f"Error: '{args.image}' not found.")
        sys.exit(1)

    # ── Auth ──────────────────────────────────────────────────────────────────
    credentials    = get_credentials(args.login)
    storage_client = storage.Client(project=args.project, credentials=credentials)

    ts         = int(time.time())
    blob_name  = f"loader-{ts}.tar.gz"
    image_name = f"miniosv-{args.arch}-{ts}"
    vm_name    = f"miniosv-{args.arch}-{ts}"

    # 1. Prepare tar.gz
    with tempfile.TemporaryDirectory() as tmp:
        tar_path = prepare_tar(args.image, tmp)
        # 2. Upload to GCS (tar.gz deleted when tmp dir is removed)
        bucket_name = ensure_bucket(storage_client, args.project)
        gcs_uri     = upload_to_gcs(storage_client, bucket_name, tar_path, blob_name)

    # 3. Custom image
    image_link = create_image(args.project, image_name, gcs_uri, args.arch, credentials)

    # 4. VM
    launch_vm(args.project, vm_name, image_link, args.arch, credentials)

    machine = ARCH_CONFIG[args.arch]["machine_type"]
    print(f"\n[done] VM: {vm_name} ({machine}) in {ZONE}")

    # 5. Wait for VM to produce serial output then fetch it
    print(f"\n[serial] Waiting for VM to boot and produce serial output…")
    time.sleep(30)  # give the VM time to start writing to the serial port
    subprocess.run(
        [
            "gcloud", "compute", "instances", "get-serial-port-output", vm_name,
            f"--zone={ZONE}",
            f"--project={args.project}",
        ],
        check=False,  # don't fail the script if serial output is empty yet
    )

    # 6. Cleanup — delete VM, image, and GCS blob
    print(f"\n[cleanup] Deleting VM '{vm_name}'…")
    subprocess.run(
        [
            "gcloud", "compute", "instances", "delete", vm_name,
            f"--zone={ZONE}",
            f"--project={args.project}",
            "--quiet",
        ],
        check=True,
    )

    print(f"[cleanup] Deleting image '{image_name}'…")
    subprocess.run(
        [
            "gcloud", "compute", "images", "delete", image_name,
            f"--project={args.project}",
            "--quiet",
        ],
        check=True,
    )

    print(f"[cleanup] Deleting GCS blob {gcs_uri}…")
    subprocess.run(
        ["gcloud", "storage", "rm", gcs_uri],
        check=True,
    )

    print(f"[cleanup] Done. All resources removed.")


if __name__ == "__main__":
    main()
