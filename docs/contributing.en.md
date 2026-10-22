# Contributing

[Project home](../README.md) · [Development](en/development.md) · [简体中文](contributing.md)

Code, documentation, translations, reproducible reports, and real-device validation all help improve Foundation.

## Report a problem

Check the [FAQ](en/faq.md) and the target version's [release notes](https://github.com/AlkaidLab/foundation-sunshine/releases), then submit through a [GitHub issue template](https://github.com/AlkaidLab/foundation-sunshine/issues/new/choose) with:

- Windows version and build number.
- Foundation Sunshine, desktop GUI, and Moonlight client versions.
- GPU, drivers, and relevant device models.
- Reproducible steps, expected behavior, and actual behavior.
- Logs from the relevant time, errors, and useful screenshots.

For HDR, include the display and negotiated format; for audio, the output device, channels, and codec; for controllers, the connection method, virtual type, and component status. Report client installation, store distribution, and client-only issues to the client project.

## Submit code

Submit host and WebUI changes to this repository, and desktop GUI changes to [sunshine-control-panel](https://github.com/qiin2333/sunshine-control-panel). Link coordinated PRs and identify the commits or build combination required.

1. Describe a concrete problem or use case and keep the change scoped.
2. Follow [development](en/development.md) and run checks relevant to the change.
3. Explain resulting behavior, verification methods and results, and conditions not yet tested.
4. Update user documentation, GUI text, and corresponding languages for new user features.

Lead PR titles and descriptions with observable behavior. Add implementation details needed to review protocol, driver, capture, or lifecycle changes.

## Documentation and translation

### Where content belongs

| Content | Location |
| --- | --- |
| Highlights, downloads, and complete first connection | Root README |
| Capability, activation, and confirmation | `docs/en/features.md` and `docs/zh-CN/features.md` |
| Symptoms, fixes, and reporting details | Language-specific `faq.md` |
| Updates and configuration preservation | Language-specific `upgrading.md` |
| Environment, code, and verification entry points | Language-specific `development.md` |
| Contribution workflow | Root CONTRIBUTING |

### Writing and formatting

Use one H1 per page, H2 for main tasks, and H3 for necessary substeps. Use numbered lists for actions, compact tables for comparisons, and code formatting for paths and commands. Keep required first-use steps visible; reserve collapsible sections for secondary details.

Explain what a feature does, where to enable it, and what confirms success. Keep hardware, component, and client conditions next to the feature. Avoid unsupported performance figures and claims that every client supports every extension.

README, user guides, and contribution entry points have corresponding Chinese and English pages. Use matching filenames and directory structure, with back and language links at the top. Update both languages together when changing downloads, steps, or capability requirements. Adapt playful wording naturally while keeping instructions precise.

## Share device validation

Record host and client versions, hardware, connection method, settings, and actual results. Document HDR negotiation, host metadata injection, and terminal presentation separately. Test controller input, triggers, and haptics as distinct capabilities. This makes compatibility evidence reviewable.
