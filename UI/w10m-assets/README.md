# Windows 10 Mobile shell pictures

The pictures of the Windows 10 Mobile style shell, from **windows-10-mobile-lvgl** by Felix Biego (MIT licence; `NOTICE` here has the licence text, the upstream commit and the list of what was converted, `THIRD_PARTY.md` at the top of the repository credits it). One file per picture; the file name without `.png` is the name the shell looks up (`UI/PACK-NAMES.md` lists every name and where it is drawn).

| Name | What | Size |
|---|---|---|
| `wp_settings`, `wp_system`, `wp_devices`, `wp_network`, `wp_personalization`, `wp_apps`, `wp_account`, `wp_time`, `wp_privacy`, `wp_about` | the Settings tile and the nine Settings sections | 32 px |
| `wifi_icon` | the status bar's network icon | 24 px |
| `wifi_ic`, `brightness_ic`, `settings_ic`, `airplane_ic`, `cellular_ic`, `bluetooth_ic`, `battery_ic`, `hotspot_ic`, `vpn_ic`, `location_ic` | the quick actions | 16 px |
| `img0` to `img9` | the ten wallpapers (also used by the Android shell) | 320 x 569 |

Made with `tools/ui/w10m_pack.py`, which fetches upstream into a temporary folder, copies or converts the pictures and packs them with purrstrap's packer into `out/ui/pack-w10m.bin` (3.7 MB with the wallpapers, 21 required names, no required name missing). Pack the folder again without fetching:

    python3 -I tools/ui/w10m_pack.py --no-fetch

Most of this art is Windows-style and the origin of the wallpapers is not stated upstream: it is placeholder art (`THIRD_PARTY.md`), to be replaced by original art before release. Anything new is original.
