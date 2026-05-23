# AtomicRipper Windows Store Package

This folder builds an MSIX package for the Microsoft Store/Partner Center without changing the normal CMake project.

Latest packaged MSIX:
[AtomicRipper_0.7.3.0_x64.msix](https://github.com/punktilend/AtomicRipper/releases/download/v0.7.3.0/AtomicRipper_0.7.3.0_x64.msix)

## Build the package

From the repo root:

```powershell
.\windows-store\Build-StorePackage.ps1
```

The output package is written to:

```text
windows-store\out\AtomicRipper_0.7.3.0_x64.msix
```

The script:

- Builds the normal Release binaries with the existing CMake preset.
- Copies the GUI, CLI, Qt runtime, and dependency DLLs into a clean staging folder.
- Replaces the staged executables' embedded manifest with `Application.manifest` so the Store build launches as the current user.
- Generates required Store tile/logo PNG assets.
- Writes `AppxManifest.xml` from `Package.appxmanifest.in`.
- Runs the Windows SDK `makeappx.exe`.

## Before Store upload

In Partner Center, reserve the app name and copy the exact package identity values into the script arguments:

```powershell
.\windows-store\Build-StorePackage.ps1 `
  -PackageIdentityName "YOUR_PARTNER_CENTER_PACKAGE_NAME" `
  -Publisher "CN=YOUR_PUBLISHER_ID" `
  -PublisherDisplayName "Your Publisher Name" `
  -Version "0.7.3.0"
```

For local sideload testing you can sign the MSIX with a trusted certificate:

```powershell
.\windows-store\Build-StorePackage.ps1 -Sign -CertificatePath C:\path\to\cert.pfx
```

Partner Center signing is handled by Microsoft after upload, but the package identity in the manifest must match your reserved Store identity.

## Store-readiness notes

AtomicRipper is packaged as a full-trust desktop app because it is a native Qt/Win32 application.

The normal desktop build embeds a `requireAdministrator` application manifest. This packaging script changes only the staged Store copy to `asInvoker` by default. That is better for Store certification, but raw CD ripping may still need a follow-up app change so the GUI can request elevation only for the specific operation that needs it. To keep the original embedded manifest in the MSIX for testing, pass `-KeepEmbeddedManifest`.

The manifest requests:

- `internetClient` for MusicBrainz, cover art, AccurateRip, and other network lookups.
- `runFullTrust` for the packaged desktop app entry point.
