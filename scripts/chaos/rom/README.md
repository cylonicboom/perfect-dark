# Chaos model-swap overlay ROM

Drop a **full Perfect Dark `.z64` ROM** (same region as your base ROM — e.g. a
"Mario characters" model mod) into this folder. On boot, `chaos.lua` auto-loads
the first ROM-sized file it finds here as a model-swap overlay.

Then trigger the **"Model Swap"** Chaos effect (Chaos Alpha menu / `/chaos
trigger model_swap`): every character body + head is sourced from this ROM.
Models swap as characters respawn.

- Any filename works; the loader picks the first 32 MB file in this folder.
- Nothing is committed to git from here (ROMs are gitignored).
- No ROM here = the Model Swap effect is simply disabled (no error).
