# bare-metal-diffusion
<img width="1024" height="1024" alt="bare-meta-llama" src="https://github.com/user-attachments/assets/5ee41553-b088-4352-88f7-e3c1c22be8d0" />

The goal for these two demos is to show that operating systems, frameworks, and firmwares are all optional in the era of AI generated code. 

Two builds of the Heat2D demo live side by side:

- `uefi/` — the original UEFI app that runs before the OS on Raspberry Pi 5 (utilizes the firmware API) 
- `metal/` — a bare-metal Circle-based build for Raspberry Pi 5 with improved typography and runtime-selectable color schemes.

Each folder contains its own README with build and deployment instructions.

Heat transfer simulation through a 2D heat sink 
<img width="1389" height="1079" alt="Screenshot 2025-12-27 143555" src="https://github.com/user-attachments/assets/3acd0b38-5250-46bc-ac6e-b8edda36129b" />

Heat transfer simulation through a conductive plate with 3 sources 
<img width="1358" height="1032" alt="Screenshot 2025-12-27 010928" src="https://github.com/user-attachments/assets/40032e58-6c8d-43f2-beac-1887acb2ff18" />
