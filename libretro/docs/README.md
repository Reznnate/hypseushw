# libretro documentation page

`hypseushw.md` is this core's page for libretro's documentation site
(https://github.com/libretro/docs), written from libretro's core template
(`docs/meta/core-template.md`).  Keep it in step with the core options and
controls in `lr_core.cpp`.

To publish it, copy it to `docs/library/hypseushw.md` in a fork of
libretro/docs and add it to `mkdocs.yml`, in the *Arcade Emulation* list:

```yaml
        - 'Arcade (Daphne HypseusHW)': 'library/hypseushw.md'
```
