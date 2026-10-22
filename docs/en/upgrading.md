# Upgrading

[Project home](../readme/README.en.md) · [Documentation](README.md) · [简体中文](../zh-CN/upgrading.md)

## Choose a release

Use the [latest regular release](https://github.com/AlkaidLab/foundation-sunshine/releases/latest) for daily use. To investigate a specified issue or try new features, choose a version marked **Pre-release** in the [release list](https://github.com/AlkaidLab/foundation-sunshine/releases) and read its notes first.

The header Latest badge includes pre-releases, while the host download button points to the regular release. Their versions may differ.

## Preserve configuration

End remote sessions and record the installed version. Back up the configuration directory in use. The default Windows installation usually uses `%ProgramFiles%\Sunshine\config`; custom installations or startup arguments may use another path. Back up the whole directory, including application and pairing state, rather than only `sunshine.conf`.

## Install and check

1. Check for updates in the desktop GUI or download the target release's `WindowsInstaller.exe`.
2. End active sessions and install, preserving configuration when prompted. Portable users should follow that release's instructions. Keep optional components matched to their expected versions.
3. Open the GUI and check the version, applications, paired devices, and component status.
4. Connect to Desktop from a client, verify video, input, and audio, then test the feature affected by the update.

A changed version number confirms installation information; also complete a real connection.

## Problems after updating

Save logs and reproduction steps and check known issues in the release notes. If reverting, preserve the current configuration first and restore the previous package with matching configuration according to its notes. See [contributing](../contributing.en.md) for reporting details.
