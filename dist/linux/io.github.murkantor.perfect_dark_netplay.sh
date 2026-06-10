#!/usr/bin/env bash
# Flatpak launcher for Perfect Dark Netplay.
#
# Inside the sandbox XDG_DATA_HOME is the app's private data dir
# (~/.var/app/io.github.murkantor.perfect_dark_netplay/data on the host).
# Drop your ROM into its roms/ subdir:
#   ~/.var/app/io.github.murkantor.perfect_dark_netplay/data/roms/pd.ntsc-final.z64
# Saves and pd.ini live in saves/.

set -u

# Create user data directories if necessary
for path in roms saves mods; do
    if [ ! -d "${XDG_DATA_HOME}/${path}" ]; then
        mkdir -p "${XDG_DATA_HOME}/${path}"
    fi
done

# Default to the NTSC binary; dispatch to a JPN/PAL build if that ROM is
# present AND the matching binary was built into this flatpak (the shipped
# manifest builds NTSC only — see the yaml to add the others).
executable="pd.x86_64"
if [ -f "${XDG_DATA_HOME}/roms/pd.ntsc-final.z64" ] || \
   [ -f "${XDG_DATA_HOME}/roms/pd.ntsc-1.0.z64" ]; then
    executable="pd.x86_64"
elif [ -f "${XDG_DATA_HOME}/roms/pd.jpn-final.z64" ] && command -v pd.jpn.x86_64 >/dev/null 2>&1; then
    executable="pd.jpn.x86_64"
elif [ -f "${XDG_DATA_HOME}/roms/pd.pal-final.z64" ] && command -v pd.pal.x86_64 >/dev/null 2>&1; then
    executable="pd.pal.x86_64"
fi

# If the first parameter is one of the pd executables, run that regardless of
# ROM auto-detection and pass the remaining parameters to it; otherwise pass
# all arguments through.
case "${1:-}" in
    "pd."*)
        executable="$1"
        shift
        ;;
esac

exec "$executable" --basedir "${XDG_DATA_HOME}/roms" --savedir "${XDG_DATA_HOME}/saves" "$@"
