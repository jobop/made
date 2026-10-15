# Marketplace Capability Split

The three marketplaces each own one layer and do package management only, decoupled from the made firmware and the bridge runtime: a package takes effect once installed into the bridge. The plugin marketplace controls made itself (themes, governance, agent runtimes); skills do not get their own marketplace in this repo — use the existing skills marketplace and attach them to agents to extend capabilities; the expert marketplace only injects an agent's personality. For classification, boundaries and no-mixing rules, see [Marketplace split](../MARKETS.md). Classify any new marketplace, plugin, skill or expert accordingly. The plugin, theme and example directories already inside the bridge are runtime locations — do not move them into marketplace directories.

## Install and publish only via the marketplace CLI (hard rule)

To install packages into `~/made/{plugins,themes}` or publish packages to the marketplace, always use the marketplace's official CLI — never bypass it:

```sh
npx made-market install <package ID>      # install (add --force to overwrite an existing one)
npx made-market validate <package dir>    # local pre-publish check, same rules as the marketplace server
npx made-market publish <package dir>     # publish (login required first)
npx made-market info <package ID>         # version, checksum and install location
```

Why: the CLI ships with sha256 verification, a live index and version registration. Hand-copying files, running the repo's CLI scripts directly, or uploading archives manually bypasses verification and registration — what you install ends up not being the same thing as what's in the marketplace.

Two easy pitfalls:

- `install` **rebuilds the target directory from scratch** (rmSync, then writes from the archive). The user's existing `settings.json` gets overwritten by the package defaults — back up keys and endpoint config before installing, restore after.
- `install` fetches from the marketplace's live index, not from a local source directory. After editing plugin source, `publish` a new version first; only then will `install` pick up the new code. Judge the installed version by sha256, never by file timestamps.
