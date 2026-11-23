# Project boundaries

- This directory is the Synology / TrueNAS application layer on LLLED_FPK branch synology-truenas. Keep fnOS packaging at the repository root.
- BIOS, fan and AC-recovery hardware capabilities must come from E:\工作台\UGREEN-NAS-Hardware and its pinned Almighty release. Implement or verify low-level capabilities upstream, never duplicate them here.
- All LED operations must go through linux/app/server/bin/ugreen_leds_cli (runtime server/bin/ugreen_leds_cli). Do not call ugreenctl LED subcommands or implement direct hardware writes in this application.
- Keep BIOS and LED control planes separate.
- No real hardware writes during tests. Use fixtures or explicit user-authorized hardware validation.
- Keep immutable upstream payload bytes and verify them against scripts/hardware-release.json.
- Write final distributable packages and checksums only to the repository root dist/ (not linux/dist/).
- Packaging, pushing and publishing are separate operations; follow the user's explicit scope.
- Do not claim Linux or TrueNAS support until the deployment and hardware capability matrix have been tested.
