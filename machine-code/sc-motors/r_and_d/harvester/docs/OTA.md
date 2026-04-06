# OTA

The recommended approach is app-level OTA for the Pi-hosted SC runtime.

Do not start with full-device image OTA.

## Approach

Package the harvester runtime as a versioned application bundle.

Install each release into its own directory and switch the active version atomically.

Use `systemd` to run the service.

Keep persistent data outside the release directory.

## Layout

```text
/opt/rooted/harvester/
  releases/
    1.0.0/
    1.0.1/
  current -> releases/1.0.1

/var/lib/rooted/harvester/
  config/
  presets/
  state/
```

## Update flow

1. Download the new signed artifact.
2. Verify signature and checksum.
3. Unpack into a new release directory.
4. Run preflight checks.
5. Atomically switch `current` to the new release.
6. Restart the `systemd` service.
7. Run a health check.
8. Commit the release or roll back to the previous version.

## Rollback

Rollback should restore:

- previous release symlink
- previous service version

Rollback should not overwrite:

- config
- presets
- persistent state
- logs

## Versioning

Track runtime compatibility with ClearCore firmware explicitly.

Each runtime release should declare:

- runtime version
- ClearCore protocol version
- minimum supported ClearCore firmware version

The Pi runtime should fail clearly if the connected ClearCore firmware is incompatible.

## ClearCore

ClearCore firmware updates should stay separate from Pi runtime OTA.

They should be less frequent and version-checked against the runtime.

## Future

If we later need stronger fleet management or full-device rollback, move to a dedicated OTA framework such as Mender or RAUC.
