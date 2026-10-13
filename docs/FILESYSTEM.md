<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Filesystem

`src/nx/hle/services/fs_tree.{c,h}` indexes `<Game>_data/` once, capturing its absolute base.
Subsequent game-data lookup, metadata and enumeration use the tree; content I/O requires indexed nodes.
No host rediscovery. External edits require restart.

Synthetic tree + IPC/RomFS checks, without game data/GPU:

```bash
python3 tests/run_filesystem_tree.py --cc clang --sanitize
```

Windows: `py -3`; `--cc` accepts a quoted path. Harness selection: `--only tree` / `--only service`.

## Paths

Top-level directories are drives; global lookup accepts data-relative paths:

| Guest path | Data-directory path |
| --- | --- |
| `drive:/file` | `drive/file` |
| `/file` | `root/file` |

`IFileSystem`: mount-relative `/`; reject drive prefixes and escaping `..`. Normalize separators/`.`; case-sensitive.
Missing `root/` is virtual until mutation. Saves use `save/<type>`; Account save adopts nonempty `save/system`
if `save/user` is absent.

Init rejects scan/stat errors, symlinks/junctions, regular-file hardlinks, special files and invalid names.
Tree limits: 4096-byte path buffers including NUL, 256 directory levels below the data root.
IPC requests allow 0x300-byte paths excluding NUL. Host limits also apply.

## State + I/O

- `FsTree.lock`: tree access, service I/O, shared size cache. Resolve first; record successful host mutations,
  including partial success.
- Handles retain node identity across rename; detached nodes live until last close. Removed directory handles return `FS_NOTFOUND`.
- Live directory cursor: `strcmp` order, last visited name, latched EOF; counts reflect current children.
- RomFS: prefer indexed `romfs.bin`; otherwise synthesize from `romfs/`.
- Readonly RAM copies: missing system archives become valid empty images at `system/archives/<16-hex-id>.romfs`; OS fallback at
  `fonts/SystemFallback.ttf`. No persistent host copy.
