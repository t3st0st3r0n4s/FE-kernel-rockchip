# OPi5U A14 — KernelSU Next update policy

This branch integrates upstream [KernelSU Next](https://github.com/KernelSU-Next/KernelSU-Next)
in built-in mode (`CONFIG_KSU=y`) using the `KernelSU-Next` Git submodule
and the `drivers/kernelsu` symlink.

## Version policy

- Select the **latest published stable GitHub release** when preparing an update.
  GitHub's `releases/latest` excludes drafts and prereleases.
- Do not float the submodule to `dev`, automatically update it during kernel
  compilation, or hardcode a specific release permanently in the build.
- Record the selected release as a **submodule commit SHA**. This makes each
  kernel revision reproducible and keeps untested upstream changes out of builds.
- Verify the host build and the Orange Pi 5 Ultra board before promoting an
  updated KernelSU revision. Keep the prior qualified commit as rollback.

Initial integration: **v3.4.2**, upstream commit
`4c706b5938665866ebe16944a481a7b91e3396af`.
This is the latest stable release as checked on 2026-10-20, **not** a permanent
version ceiling. Integration is not yet host-built or board-qualified.

## Updating to the latest stable release

Run from a checkout of the `feature/kernelsu-next` branch:

```sh
git submodule update --init KernelSU-Next
tag="$(curl -fsSL https://api.github.com/repos/KernelSU-Next/KernelSU-Next/releases/latest |
    python3 -c 'import json,sys; r=json.load(sys.stdin); assert not r["draft"] and not r["prerelease"]; print(r["tag_name"])')"
git -C KernelSU-Next fetch origin "refs/tags/$tag:refs/tags/$tag"
git -C KernelSU-Next checkout --detach "$tag"
git add KernelSU-Next
git diff --cached --submodule=short -- KernelSU-Next
```

If the submodule did not change, there is nothing to commit. Otherwise commit
the updated submodule pointer, then perform the existing A14 incremental kernel
build and board validation before promotion. Do not run `git submodule update
--remote`: the upstream default branch is not the stable-release policy.

For new checkouts, `git submodule update --init KernelSU-Next` restores the
exact version recorded by the kernel commit.
