# Release Signing

This document describes the signing policy enforced by
`.github/workflows/release.yml` and the repository configuration it requires.

## What is signed

- **RPM packages:** signed on every `v*` tag. The workflow refuses to publish
  an unsigned RPM: if the signing key is missing or a signature cannot be
  verified, the job fails.
- **DEB packages:** not individually signed. apt does not verify per-package
  signatures, and the project publishes release files rather than an apt
  repository. Release checksums cover integrity. If an apt repository is added
  later, sign its `InRelease` metadata instead of individual `.deb` files.

The workflow also checks that the pushed tag matches `BNIO_VERSION` in
`CMakeLists.txt` before building and signing.

## Required repository secrets

Add these under **Settings → Secrets and variables → Actions**:

| Secret | Required | Contents |
|---|---|---|
| `RPM_GPG_PRIVATE_KEY` | Yes | ASCII-armored private signing key (`gpg --armor --export-secret-keys <KEY_ID>`) |
| `RPM_GPG_PASSPHRASE` | No | Passphrase for the signing key; omit if the CI key is unprotected |

The key ID is derived from the imported secret key; no separate key-ID secret
is needed. The workflow exports the matching public key as
`bnio-rpm-signing-key.asc` and a `RPM-SIGNING.txt` fingerprint file, and
attaches both to the GitHub Release.

### Generating a dedicated key

Use a dedicated signing key, not a personal key:

```sh
gpg --quick-generate-key "bnio release signing <release@example.org>" \
  rsa4096 sign never
gpg --armor --export-secret-keys "<KEY_ID>" > rpm-signing-key.asc
```

Store `rpm-signing-key.asc` in a secrets manager and add its contents as
`RPM_GPG_PRIVATE_KEY`. Publish only the public key:

```sh
gpg --armor --export "<KEY_ID>" > bnio-rpm-signing-key.asc
```

For a stricter setup, use a signing subkey and keep the primary key offline.
Record the fingerprint and a revocation procedure before the first release.

## Verifying a release

```sh
rpm --import bnio-rpm-signing-key.asc
rpmkeys --checksig bnio-<version>-Linux-runtime.rpm
rpmkeys --checksig bnio-<version>-Linux-development.rpm
```

`rpmkeys --checksig` must report `digests signatures OK` for both packages.
For DEB artifacts, verify the published SHA-256 checksums.

## How the workflow signs

The signing step runs only for `v*` tags, on the Ubuntu runner, after CPack
produces the RPMs:

1. Imports `RPM_GPG_PRIVATE_KEY` into a temporary `GNUPGHOME`.
2. If `RPM_GPG_PASSPHRASE` is set, presets it into the temporary gpg-agent with
   `gpg-preset-passphrase` (the agent is started with
   `allow-preset-passphrase`). Unprotected keys skip this step.
3. Signs every `artifacts/*.rpm` with `rpmsign --addsign`, using
   `_gpg_digest_algo=sha256` and `__gpg=/usr/bin/gpg` (Ubuntu installs `gpg`,
   not `gpg2`).
4. Imports the public key into the local rpm database and verifies every
   package with `rpmkeys --checksig`; any failure fails the job.
5. Exports the public key and writes `RPM-SIGNING.txt` into the uploaded
   `packages` artifact.

The same commands have been verified against Fedora 45 (rpm 6.1 with
`_pkgverify_level=all`) and Rocky Linux 10 (rpm 4.19); both accept the
RSA/SHA256 header signature produced by Ubuntu's `rpmsign`.

## Key management

- Keep the private key offline or in a secrets manager/HSM; never commit it or
  paste it into workflow files.
- Use a passphrase-protected key and store the passphrase only as
  `RPM_GPG_PASSPHRASE`. An unprotected key is acceptable only as a temporary
  bootstrap.
- Prefer separate keys for RPM and DEB/repository signing so that a compromise
  or revocation affects one trust domain.
- Publish the fingerprint in the repository and in each release. Rotate keys on
  a schedule and publish a signed revocation statement if a key is compromised.
- The test keys used during ABI verification were disposable and never left
  the test containers; they must not be reused for releases.
