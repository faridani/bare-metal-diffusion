# uefi-2D-heat-diffusion

The original Raspberry Pi 5 UEFI Heat2D demo now lives entirely in this `uefi/` folder. The code, build scripts, and walkthrough (`Heat2D_RPi5_UEFI_Heat_Diffusion_Demo_with_QEMU.md`) are unchanged other than the new location.

Quick start:

- Copy `Heat2D.inf`, `Heat2D.c`, and the companion helper files from this directory into your EDK2 workspace.
- Follow the step-by-step guide in `Heat2D_RPi5_UEFI_Heat_Diffusion_Demo_with_QEMU.md` for build, SD-card layout, and QEMU tips.
- Outputs (e.g., `Heat2D.efi`) should now be referenced relative to `uefi/` when copying or editing files in this repo.
