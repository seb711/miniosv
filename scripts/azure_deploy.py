#!/usr/bin/env python3
"""
Azure deploy script for MiniOSv UEFI kernel.

Flow:
    loader.img → fixed VHD → blob upload → managed disk → image → VM
    → wait → fetch serial log → cleanup (VM, NIC, image, disk, blob)

Usage:
    python3 azure_deploy.py --login                    # fresh device-code login (~1hr token)
    python3 azure_deploy.py <image> <arch>             # deploy
    python3 azure_deploy.py --login <image> <arch>

    <arch>  x86  → Standard_D2s_v3  (Intel, x64)
            arm  → Standard_D2ps_v5 (Ampere, Arm64)

Prerequisites:
    pip install azure-identity azure-mgmt-compute azure-mgmt-network azure-storage-blob

Examples:
    python3 azure_deploy.py --login loader.img x86
    python3 azure_deploy.py --login loader.img arm
"""

import sys
import os
import time
import subprocess
import argparse
import tempfile

from azure.identity import DeviceCodeCredential
from azure.mgmt.compute import ComputeManagementClient
from azure.mgmt.network import NetworkManagementClient
from azure.storage.blob import BlobServiceClient

# ── Config ────────────────────────────────────────────────────────────────────
#
# Account-specific identifiers come from the environment so they stay out of
# version control. Set these before running (see README / your shell profile):
#
#     export AZURE_SUBSCRIPTION_ID=<your-subscription-guid>
#     export AZURE_TENANT_ID=<your-tenant-guid>
#     export AZURE_STORAGE_ACCOUNT=<your-storage-account-name>
#
# RESOURCE_GROUP / LOCATION / CONTAINER_NAME may also be overridden via env.

def _require_env(name: str) -> str:
    val = os.environ.get(name)
    if not val:
        sys.exit(f"error: environment variable {name} is not set "
                 f"(see the Config section of {os.path.basename(__file__)})")
    return val

SUBSCRIPTION_ID = _require_env("AZURE_SUBSCRIPTION_ID")
TENANT_ID       = _require_env("AZURE_TENANT_ID")
STORAGE_ACCOUNT = _require_env("AZURE_STORAGE_ACCOUNT")
RESOURCE_GROUP  = os.environ.get("AZURE_RESOURCE_GROUP", "miniosv-rg")
LOCATION        = os.environ.get("AZURE_LOCATION", "eastus")
CONTAINER_NAME  = os.environ.get("AZURE_CONTAINER_NAME", "images")
DISK_SIZE_GB    = 10

# Per-arch config — same interface as gcp_deploy.py
ARCH_CONFIG = {
    "x86": {
        "vm_size":    "Standard_D2s_v3",
        "az_arch":    "x64",
    },
    "arm": {
        "vm_size":    "Standard_D2ps_v5",
        "az_arch":    "Arm64",
    },
}

# ── Auth — device code only, no local credentials ─────────────────────────────

def get_credential(force_login: bool):
    """
    Always uses DeviceCodeCredential — no az CLI tokens, no service principal,
    nothing stored on disk permanently. Token lives only in memory (~1hr).

    --login  → triggers the browser flow immediately so you authenticate
               before the deploy starts.
    No flag  → triggers on the first API call if no token exists yet.
    """
    cred = DeviceCodeCredential(tenant_id=TENANT_ID)
    if force_login:
        print("[auth] Starting device-code login…")
        print("[auth] Open the URL below and enter the code in your browser.")
        print()
        cred.get_token("https://management.azure.com/.default")
        print()
        print("[auth] Login successful. Token valid for ~1 hour.")
    return cred

# ── 1. Prepare ────────────────────────────────────────────────────────────────

def prepare_vhd(src: str, work_dir: str) -> str:
    """Convert loader.img (raw) to a fixed-size VHD. Azure rejects everything else."""
    dst = os.path.join(work_dir, "disk.vhd")
    print(f"[prepare] Converting {src} → fixed VHD…")
    subprocess.run(
        ["qemu-img", "convert", "-O", "vpc", "-o", "subformat=fixed,force_size", src, dst],
        check=True,
    )
    print(f"[prepare] Done ({os.path.getsize(dst) / (1024**2):.1f} MiB)")
    return dst

# ── 2. Upload ─────────────────────────────────────────────────────────────────

def upload_vhd(credential, vhd_path: str, blob_name: str) -> str:
    """
    Upload as a page blob — required for VHDs.
    Skips empty pages (sparse optimization, same as the AWS script).
    Returns the blob URL.
    """
    svc  = BlobServiceClient(f"https://{STORAGE_ACCOUNT}.blob.core.windows.net", credential=credential)
    blob = svc.get_container_client(CONTAINER_NAME).get_blob_client(blob_name)

    file_size    = os.path.getsize(vhd_path)
    aligned_size = ((file_size + 511) // 512) * 512  # page blobs must be 512-byte aligned

    print(f"[upload] {vhd_path} → https://{STORAGE_ACCOUNT}.blob.core.windows.net/{CONTAINER_NAME}/{blob_name}")
    blob.create_page_blob(size=aligned_size)

    chunk_size = 4 * 1024 * 1024  # 4 MiB
    offset     = 0

    with open(vhd_path, "rb") as f:
        while True:
            data = f.read(chunk_size)
            if not data:
                break
            if len(data) % 512 != 0:
                data += b'\x00' * (512 - len(data) % 512)
            if any(data):  # skip empty pages
                blob.upload_page(data, offset=offset, length=len(data))
            offset += len(data)
            print(f"\r[upload] {offset / (1024**2):.1f} / {file_size / (1024**2):.1f} MiB ({offset/aligned_size*100:.1f}%)", end="", flush=True)

    print(f"\n[upload] Done.")
    return blob.url

# ── 3. Managed disk ───────────────────────────────────────────────────────────

def create_managed_disk(compute: ComputeManagementClient, disk_name: str,
                        blob_url: str, arch: str) -> str:
    """Import the VHD blob as a managed disk. Gen V2 = UEFI."""
    print(f"[disk] Creating managed disk '{disk_name}'…")
    disk = compute.disks.begin_create_or_update(
        RESOURCE_GROUP, disk_name,
        {
            "location": LOCATION,
            "os_type": "Linux",
            "hyper_v_generation": "V2",
            "supported_capabilities": {"architecture": ARCH_CONFIG[arch]["az_arch"]},
            "disk_size_gb": DISK_SIZE_GB,
            "creation_data": {
                "create_option": "Import",
                "source_uri": blob_url,
                "storage_account_id": (
                    f"/subscriptions/{SUBSCRIPTION_ID}/resourceGroups/{RESOURCE_GROUP}"
                    f"/providers/Microsoft.Storage/storageAccounts/{STORAGE_ACCOUNT}"
                ),
            },
        },
    ).result()
    print(f"[disk] Ready: {disk.id}")
    return disk.id


# ── 5. VM ─────────────────────────────────────────────────────────────────────

def launch_vm(compute: ComputeManagementClient, network: NetworkManagementClient,
              vm_name: str, disk_id: str, arch: str) -> tuple:
    """
    Launch a VM with a minimal private-only NIC (no public IP, no cost).
    Boots directly from managed disk — works for both x86 and arm.
    Returns (None, nic_name) — non-blocking launch.
    """
    cfg = ARCH_CONFIG[arch]

    # ── VNet + subnet (free, reused across deploys) ───────────────────────────
    vnet_name = "miniosv-vnet-us"
    print(f"[network] Ensuring VNet '{vnet_name}'…")
    vnet = network.virtual_networks.begin_create_or_update(
        RESOURCE_GROUP, vnet_name,
        {
            "location": LOCATION,
            "address_space": {"address_prefixes": ["10.0.0.0/16"]},
            "subnets": [{"name": "default", "address_prefix": "10.0.0.0/24"}],
        },
    ).result()
    subnet_id = vnet.subnets[0].id

    # ── NIC — private IP only, no public IP (free) ────────────────────────────
    nic_name = f"{vm_name}-nic"
    print(f"[network] Creating NIC '{nic_name}' (private only)…")
    nic = network.network_interfaces.begin_create_or_update(
        RESOURCE_GROUP, nic_name,
        {
            "location": LOCATION,
            "ip_configurations": [{
                "name": "ipconfig1",
                "subnet": {"id": subnet_id},
                # no public_ip_address = private only
            }],
        },
    ).result()

    # ── VM ────────────────────────────────────────────────────────────────────
    print(f"[vm] Launching {cfg['vm_size']} VM '{vm_name}' ({arch})…")
    poller = compute.virtual_machines.begin_create_or_update(
        RESOURCE_GROUP, vm_name,
        {
            "location": LOCATION,
            "hardware_profile": {"vm_size": cfg["vm_size"]},
            "storage_profile": {
                "os_disk": {
                    "create_option": "Attach",
                    "managed_disk": {"id": disk_id},
                    "os_type": "Linux",
                    "delete_option": "Delete",
                },
            },

            "network_profile": {
                "network_interfaces": [{"id": nic.id, "primary": True}],
            },
            "diagnostics_profile": {
                "boot_diagnostics": {"enabled": True},  # enables serial console
            },
        },
    )
    # Return immediately without waiting — serial log fetch includes its own wait
    print(f"[vm] Provisioning started, not waiting for completion.")
    return None, nic_name

# ── 6. Serial log ─────────────────────────────────────────────────────────────

def fetch_serial_log(vm_name: str):
    """Fetch boot diagnostics serial log via az CLI."""
    print(f"\n[serial] Waiting 30s for VM to boot…")
    time.sleep(30)
    print(f"[serial] Fetching serial log…\n")
    subprocess.run(
        [
            "az", "vm", "boot-diagnostics", "get-boot-log",
            "--resource-group", RESOURCE_GROUP,
            "--name", vm_name,
        ],
        check=False,  # don't fail if log is empty yet
    )

# ── 7. Cleanup ────────────────────────────────────────────────────────────────

def cleanup(compute: ComputeManagementClient, network: NetworkManagementClient,
            vm_name: str, nic_name: str, disk_name: str, blob_name: str):
    """Delete VM, NIC, disk, and blob in the right order."""

    print(f"\n[cleanup] Deleting VM '{vm_name}'…")
    compute.virtual_machines.begin_delete(RESOURCE_GROUP, vm_name).result()

    print(f"[cleanup] Deleting NIC '{nic_name}'…")
    network.network_interfaces.begin_delete(RESOURCE_GROUP, nic_name).result()

    print(f"[cleanup] Deleting disk '{disk_name}'…")
    compute.disks.begin_delete(RESOURCE_GROUP, disk_name).result()

    print(f"[cleanup] Deleting blob '{blob_name}'…")
    svc  = BlobServiceClient(f"https://{STORAGE_ACCOUNT}.blob.core.windows.net")
    blob = svc.get_container_client(CONTAINER_NAME).get_blob_client(blob_name)
    blob.delete_blob()

    print(f"[cleanup] Done. All resources removed.")

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Deploy MiniOSv UEFI kernel to Azure")
    parser.add_argument("image", nargs="?", default="loader.img",
                        help="Disk image to deploy (default: loader.img)")
    parser.add_argument("arch", nargs="?", default="x86",
                        choices=["x86", "arm"],
                        help="x86 = Standard_D2s_v3 | arm = Standard_D2ps_v5")
    parser.add_argument("--login", action="store_true",
                        help="Do a fresh device-code login before deploying")
    args = parser.parse_args()

    if not os.path.exists(args.image):
        print(f"Error: '{args.image}' not found.")
        sys.exit(1)

    credential = get_credential(args.login)
    compute    = ComputeManagementClient(credential, SUBSCRIPTION_ID)
    network    = NetworkManagementClient(credential, SUBSCRIPTION_ID)

    ts        = int(time.time())
    blob_name = f"loader-{ts}.vhd"
    disk_name = f"loader-disk-{ts}"
    vm_name   = f"miniosv-{args.arch}-{ts}"

    # 1. Convert
    with tempfile.TemporaryDirectory() as tmp:
        vhd_path = prepare_vhd(args.image, tmp)
        # 2. Upload (VHD deleted when tmp dir is removed)
        blob_url = upload_vhd(credential, vhd_path, blob_name)

    # 3. Managed disk
    disk_id = create_managed_disk(compute, disk_name, blob_url, args.arch)

    # 4. VM — boot directly from disk, non-blocking
    vm, nic_name = launch_vm(compute, network, vm_name, disk_id, args.arch)

    cfg = ARCH_CONFIG[args.arch]
    print(f"\n[done] VM provisioning started: {vm_name} ({cfg['vm_size']}) in {LOCATION}")

    # 6. Serial log (includes 30s wait for VM to boot)
    fetch_serial_log(vm_name)

    # 7. Cleanup
    cleanup(compute, network, vm_name, nic_name, disk_name, blob_name)


if __name__ == "__main__":
    main()
