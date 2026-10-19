# Building and releasing

`.github/workflows/build-release.yml` runs for pushes, pull requests and manual
dispatches. It installs the public PS5 Payload SDK v0.44, verifies its SHA-256,
fetches the pinned headers, runs the host tests and builds a normal release ELF.
No console connection, copied fonts, firmware modules or screenshots are needed.

Every successful build uploads an artifact containing:

- `kabuki-ps5.elf`
- `SHA256SUMS.txt`
- `BUILD-INFO.txt` with the source revision and SDK version
- `RELEASE-NOTES.md`

The build rejects payloads larger than 2 MiB. Release builds never pass `--dev`.

## Preparing a release

1. Test the change on a console, finish the README and selected images, and review
   the files being committed for personal data and console resources.
2. Commit the reviewed changes, then push the branch when ready.
3. Choose a new version tag, such as `v1.0.0` or `v1.0.0-rc.1`, and push it.
4. The workflow builds that tag and creates a **draft** release with the ELF,
   checksums, build information and release notes attached.
5. Review the draft, edit its notes if needed, and publish it when ready. Publishing
   is a manual step. Re-running a tag build can update draft assets but refuses
   to replace assets on an already published release.

The workflow uses GitHub's generated `GITHUB_TOKEN`; no personal token needs to
be stored as a repository secret. Only the draft-release job gets `contents: write`.
Local GitHub authentication is needed to push the branch/tag or dispatch the
workflow through the CLI; it is not needed to compile locally.

To reproduce the CI build on Linux or WSL, install `build-essential`, `binutils`,
`clang-18`, `lld-18`, Python 3, curl and unzip, then run:

```bash
bash tools/ci/setup-sdk.sh "$PWD/build/ci-sdk"
export PS5_PAYLOAD_SDK="$PWD/build/ci-sdk/ps5-payload-sdk"
bash tools/ci/build-release.sh
```

The packaged release files are written to `dist/`. Raw build logs and SDK files
are not uploaded as release assets.
