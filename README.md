# hello_eink-app

This repository contains the `hello_eink` Zephyr application plus the app-owned board and module code.

## Location

This repository now lives directly inside the shared workspace tree at:

`/workspace/projects/hello_eink`

The surrounding shared workspace provides:

- the west manifest under `/workspace/west.yml`
- the Zephyr toolchain and container setup
- the VS Code active-project tasks and debug symlink flow

## Build inside the shared workspace

From the shared repository container shell:

```sh
bash /workspace/scripts/zephyr-project.sh set /workspace/projects/hello_eink
bash /workspace/scripts/zephyr-project.sh build
```

## Repo contents

- `boards/custom/eink_llss_esp32`: app-owned custom board definition
- `modules/wifi_prov`: app-owned Zephyr module used by this app
- `patches/`: app-specific Zephyr patches applied by the shared build helper when this project is active
- `boards/eink_llss_esp32_procpu.*`: app-specific overlay and debug config
- `src/`, `prj.conf`, `Kconfig`: application sources and configuration
